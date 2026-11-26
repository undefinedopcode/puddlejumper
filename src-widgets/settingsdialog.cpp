/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settingsdialog.h"

#include "appicon.h"
#include "colorscheme.h"
#include "colorschemeeditor.h"
#include "profiles.h"
#include "serverstore.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <qtermwidget.h>

namespace {
constexpr double kFontSizeMin = 8.0;
constexpr double kFontSizeMax = 32.0;

/*
 * The profile font size spin box can't express "no value", so one step below
 * the real range stands for Inherit. Below the range, not at its minimum, so
 * a profile can still ask for 8pt.
 */
constexpr double kInheritFontSize = kFontSizeMin - 0.5;

QStringList monoFamilies()
{
    QStringList out;
    for (const QString &f : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(f))
            out << f;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

/*
 * One line of explanation under a control, in a secondary colour. Not
 * setEnabled(false): the disabled colour is meant to say "unavailable" and is
 * too faint to read comfortably. The blend follows the palette, so it works
 * in dark themes too.
 *
 * Deliberately not word-wrapped. Hints are kept to one line, and a wrapping
 * label in a QFormLayout reserves height for wrapping it doesn't need,
 * leaving a gap above and below.
 */
QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *hint = new QLabel(text, parent);
    QPalette pal = hint->palette();
    const QColor fg = pal.color(QPalette::WindowText);
    const QColor bg = pal.color(QPalette::Window);
    const auto mix = [](int a, int b) { return (a * 6 + b * 4) / 10; };
    pal.setColor(QPalette::WindowText, QColor(mix(fg.red(), bg.red()),
                                              mix(fg.green(), bg.green()),
                                              mix(fg.blue(), bg.blue())));
    hint->setPalette(pal);
    return hint;
}

/* Spin boxes apply when editing finishes, not per keystroke: typing "25000"
 * would otherwise set 2, 25, 250 and 2500 along the way. */
template <typename Box>
Box *makeSpinBox(QWidget *parent)
{
    auto *box = new Box(parent);
    box->setKeyboardTracking(false);
    return box;
}

/* Blank stand-in so profiles without the default star still line up. */
QIcon blankIcon(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    return QIcon(pm);
}

/* The arguments field only means something once a shell program is set. */
void syncShellArgs(QLineEdit *args, bool hasShell)
{
    args->setEnabled(hasShell);
    args->setPlaceholderText(hasShell ? QObject::tr("None")
                                      : QObject::tr("Set a shell program first"));
}
} // namespace

SettingsDialog::SettingsDialog(Settings &settings, ProfileStore &profiles,
                               QWidget *parent, ServerStore *servers)
    : QDialog(parent)
    , m_settings(settings)
    , m_profiles(profiles)
    , m_servers(servers)
{
    setWindowTitle(tr("Settings"));
    setModal(true);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_sections = new QListWidget(this);
    m_sections->setFixedWidth(180);
    m_sections->setFrameShape(QFrame::NoFrame);
    m_sections->setIconSize(QSize(22, 22));
    m_sections->setSpacing(2);
    const auto addSection = [this](const QString &text, const QIcon &icon) {
        auto *item = new QListWidgetItem(icon, text, m_sections);
        item->setSizeHint(QSize(0, 34));
    };
    addSection(tr("Appearance"), appIcon("palette"));
    addSection(tr("Terminal"), appIcon("square-terminal"));
    addSection(tr("Tabs & Window"), appIcon("app-window"));
    addSection(tr("Profiles"), appIcon("users"));
    if (m_servers)
        addSection(tr("Servers"), appIcon("server"));
    outer->addWidget(m_sections);

    auto *right = new QVBoxLayout;
    right->setContentsMargins(20, 14, 20, 14);
    right->setSpacing(12);

    m_pageTitle = new QLabel(this);
    QFont titleFont = m_pageTitle->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    titleFont.setBold(true);
    m_pageTitle->setFont(titleFont);
    right->addWidget(m_pageTitle);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildAppearancePage());
    m_pages->addWidget(buildTerminalPage());
    m_pages->addWidget(buildWindowPage());
    m_pages->addWidget(buildProfilesPage());
    if (m_servers)
        m_pages->addWidget(buildServersPage());
    right->addWidget(m_pages, 1);
    alignFormLabels();

    auto *footer = new QHBoxLayout;
    footer->addWidget(makeHint(tr("Changes apply immediately and are remembered."), this), 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    footer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    right->addLayout(footer);

    outer->addLayout(right, 1);

    connect(m_sections, &QListWidget::currentRowChanged, this, [this](int row) {
        m_pages->setCurrentIndex(row);
        if (auto *item = m_sections->item(row))
            m_pageTitle->setText(item->text());
    });

    m_sections->setCurrentRow(Appearance);
    syncFromSettings();

    // Size to the tallest page rather than a hard-coded pixel count, so every
    // control stays visible on any style. adjustSize() alone is not enough:
    // Qt caps it at two-thirds of the screen, and on a short screen the
    // tallest page would be clipped and its group boxes squeezed. Ask for the
    // page's real height as a floor.
    adjustSize();
    const int contentHeight = m_pages->sizeHint().height()
                              + (height() - m_pages->height());  // + title, footer
    if (height() < contentHeight)
        resize(width(), contentHeight);
    resize(qMax(width(), 640), height());
}

void SettingsDialog::showSection(Section section)
{
    if (section == Servers && !m_servers)
        section = Appearance;
    m_sections->setCurrentRow(int(section));
    show();
    raise();
    activateWindow();
}

QFormLayout *SettingsDialog::addGroup(QVBoxLayout *page, const QString &title)
{
    auto *group = new QGroupBox(title, page->parentWidget());
    auto *form = new QFormLayout(group);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    page->addWidget(group);
    m_forms << form;
    return form;
}

void SettingsDialog::alignFormLabels()
{
    // One label width for every group on every page, so the field column
    // starts at the same x everywhere and doesn't jump between sections.
    QList<QWidget *> labels;
    int width = 0;
    for (QFormLayout *form : std::as_const(m_forms)) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
            if (QWidget *label = item ? item->widget() : nullptr) {
                labels << label;
                width = qMax(width, label->sizeHint().width());
            }
        }
    }
    // Widening a label leaves its text at the left of the extra space; keep
    // it against the field, as the form's own label alignment intends.
    for (QWidget *label : std::as_const(labels)) {
        label->setMinimumWidth(width);
        if (auto *l = qobject_cast<QLabel *>(label))
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
}

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // ---- colours -----------------------------------------------------------
    QFormLayout *colours = addGroup(layout, tr("Colours"));

    m_schemeBox = new QComboBox(page);
    // allSchemeNames() rather than the raw list: it includes the user's saved
    // schemes, which qtermwidget's cached list does not.
    m_schemeBox->addItems(ColorSchemeFile::allSchemeNames());
    connect(m_schemeBox, &QComboBox::currentTextChanged, this, [this](const QString &v) {
        m_settings.setColorScheme(v);
    });

    auto *schemeRow = new QWidget(page);
    auto *schemeLayout = new QHBoxLayout(schemeRow);
    schemeLayout->setContentsMargins(0, 0, 0, 0);
    schemeLayout->setSpacing(6);
    schemeLayout->addWidget(m_schemeBox, 1);
    m_editSchemeBtn = new QToolButton(schemeRow);
    m_editSchemeBtn->setText(tr("&Edit…"));
    m_editSchemeBtn->setIcon(appIcon("pencil"));
    m_editSchemeBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_editSchemeBtn->setToolTip(tr("Edit this scheme's colours"));
    schemeLayout->addWidget(m_editSchemeBtn);
    colours->addRow(tr("Colour scheme"), schemeRow);
    colours->addRow(QString(), makeHint(tr("Your own schemes are edited in place; "
                                           "built-in ones are copied first."), page));
    connect(m_editSchemeBtn, &QToolButton::clicked, this,
            &SettingsDialog::editColorScheme);

    // Background opacity, 30-100% so the text can never vanish entirely.
    auto *opRow = new QWidget(page);
    auto *opLayout = new QHBoxLayout(opRow);
    opLayout->setContentsMargins(0, 0, 0, 0);
    m_opacitySlider = new QSlider(Qt::Horizontal, opRow);
    m_opacitySlider->setRange(30, 100);
    m_opacitySlider->setSingleStep(5);
    m_opacitySlider->setPageStep(10);
    m_opacityLabel = new QLabel(opRow);
    m_opacityLabel->setMinimumWidth(m_opacityLabel->fontMetrics().horizontalAdvance(
        QStringLiteral("100%")) + 4);
    m_opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opLayout->addWidget(m_opacitySlider, 1);
    opLayout->addWidget(m_opacityLabel);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int pct) {
        m_opacityLabel->setText(tr("%1%").arg(pct));
        m_settings.setOpacity(pct / 100.0);
    });
    colours->addRow(tr("Background opacity"), opRow);

    // ---- font --------------------------------------------------------------
    QFormLayout *font = addGroup(layout, tr("Font"));

    m_fontBox = new QComboBox(page);
    m_fontBox->addItems(monoFamilies());
    connect(m_fontBox, &QComboBox::currentTextChanged, this, [this](const QString &v) {
        m_settings.setFontFamily(v);
    });
    font->addRow(tr("Family"), m_fontBox);
    font->addRow(QString(), makeHint(tr("Only fixed-pitch fonts are listed."), page));

    m_fontSizeBox = makeSpinBox<QDoubleSpinBox>(page);
    m_fontSizeBox->setRange(kFontSizeMin, kFontSizeMax);
    m_fontSizeBox->setSingleStep(0.5);
    m_fontSizeBox->setDecimals(1);
    m_fontSizeBox->setSuffix(tr(" pt"));
    connect(m_fontSizeBox, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_settings.setFontSize(qRound(v * 2) / 2.0);
    });
    font->addRow(tr("Size"), m_fontSizeBox);

    m_lineSpacingBox = makeSpinBox<QSpinBox>(page);
    m_lineSpacingBox->setRange(0, 12);
    m_lineSpacingBox->setSuffix(tr(" px"));
    m_lineSpacingBox->setToolTip(tr("Extra space between rows of text"));
    connect(m_lineSpacingBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setLineSpacing(v);
    });
    font->addRow(tr("Line spacing"), m_lineSpacingBox);

    m_marginBox = makeSpinBox<QSpinBox>(page);
    m_marginBox->setRange(0, 32);
    m_marginBox->setSuffix(tr(" px"));
    m_marginBox->setToolTip(tr("Empty space around the text, so it doesn't touch "
                               "the window edges"));
    connect(m_marginBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setTerminalMargin(v);
    });
    font->addRow(tr("Margin"), m_marginBox);

    // ---- rendering -----------------------------------------------------------
    QFormLayout *rendering = addGroup(layout, tr("Rendering"));

    m_boldBox = new QCheckBox(tr("Show bright colours in bold"), page);
    connect(m_boldBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBoldIntense(v);
    });
    m_italicBox = new QCheckBox(tr("Allow italic text"), page);
    connect(m_italicBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setItalic(v);
    });
    m_antialiasBox = new QCheckBox(tr("Smooth text edges (antialiasing)"), page);
    connect(m_antialiasBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setAntialias(v);
    });
    rendering->addRow(tr("Text"), m_boldBox);
    rendering->addRow(QString(), m_italicBox);
    rendering->addRow(QString(), m_antialiasBox);

    m_lineCharsBox = new QCheckBox(tr("Draw box-drawing characters itself"), page);
    connect(m_lineCharsBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setDrawLineChars(v);
    });
    rendering->addRow(tr("Lines"), m_lineCharsBox);
    rendering->addRow(QString(), makeHint(tr("Crisp lines that always join up, instead "
                                             "of the font's own glyphs."), page));

    m_bidiBox = new QCheckBox(tr("Right-to-left scripts (bidirectional text)"), page);
    connect(m_bidiBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBidi(v);
    });
    rendering->addRow(tr("Direction"), m_bidiBox);

    m_paneCardsBox = new QCheckBox(tr("Pane cards (experimental)"), page);
    m_paneCardsBox->setToolTip(tr("Draw split panes as floating panes are: a rounded, faint frame "
                                  "with a small title strip"));
    connect(m_paneCardsBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setPaneCards(v);
    });
    rendering->addRow(tr("Panes"), m_paneCardsBox);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildTerminalPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // ---- cursor --------------------------------------------------------------
    QFormLayout *cursor = addGroup(layout, tr("Cursor"));

    m_cursorShapeBox = new QComboBox(page);
    m_cursorShapeBox->addItem(tr("Block"),     QStringLiteral("0"));
    m_cursorShapeBox->addItem(tr("Underline"), QStringLiteral("1"));
    m_cursorShapeBox->addItem(tr("I-beam"),    QStringLiteral("2"));
    connect(m_cursorShapeBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setCursorShape(m_cursorShapeBox->itemData(i).toInt());
    });
    cursor->addRow(tr("Shape"), m_cursorShapeBox);

    m_blinkBox = new QCheckBox(tr("Blink while the terminal has focus"), page);
    connect(m_blinkBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBlinkingCursor(v);
    });
    cursor->addRow(tr("Blinking"), m_blinkBox);

    // ---- scrollback ----------------------------------------------------------
    QFormLayout *scroll = addGroup(layout, tr("Scrollback"));

    m_scrollbackBox = makeSpinBox<QSpinBox>(page);
    m_scrollbackBox->setRange(0, 200000);
    m_scrollbackBox->setSingleStep(500);
    m_scrollbackBox->setGroupSeparatorShown(true);
    m_scrollbackBox->setSuffix(tr(" lines"));
    m_scrollbackBox->setSpecialValueText(tr("None"));
    connect(m_scrollbackBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setScrollback(v);
    });
    scroll->addRow(tr("Keep"), m_scrollbackBox);

    m_scrollbarPosBox = new QComboBox(page);
    m_scrollbarPosBox->addItem(tr("Hidden"), QStringLiteral("none"));
    m_scrollbarPosBox->addItem(tr("Left"),   QStringLiteral("left"));
    m_scrollbarPosBox->addItem(tr("Right"),  QStringLiteral("right"));
    connect(m_scrollbarPosBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setScrollbarPosition(m_scrollbarPosBox->itemData(i).toString());
    });
    scroll->addRow(tr("Scrollbar"), m_scrollbarPosBox);

    // ---- input ---------------------------------------------------------------
    QFormLayout *input = addGroup(layout, tr("Input"));

    m_pasteConfirmBox = new QCheckBox(tr("Ask before pasting more than one line"), page);
    connect(m_pasteConfirmBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setConfirmMultilinePaste(v);
    });
    m_pasteTrimBox = new QCheckBox(tr("Remove trailing newlines from pasted text"), page);
    connect(m_pasteTrimBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setTrimPastedTrailingNewlines(v);
    });
    input->addRow(tr("Pasting"), m_pasteConfirmBox);
    input->addRow(QString(), m_pasteTrimBox);
    input->addRow(QString(), makeHint(tr("Both guard against a pasted command running "
                                         "before you have checked it."), page));

    m_flowControlWarnBox = new QCheckBox(tr("Show a notice when output is paused"), page);
    connect(m_flowControlWarnBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setFlowControlWarning(v);
    });
    input->addRow(tr("Ctrl+S"), m_flowControlWarnBox);
    input->addRow(QString(), makeHint(tr("Ctrl+S pauses output until Ctrl+Q, which "
                                         "otherwise looks like a hung terminal."), page));

#ifdef PUDDLEJUMPER_REX
    QFormLayout *rex = addGroup(layout, tr("Rex"));
    auto *scriptsRow = new QWidget(page);
    auto *scriptsLayout = new QHBoxLayout(scriptsRow);
    scriptsLayout->setContentsMargins(0, 0, 0, 0);
    m_rexScriptsEdit = new QLineEdit(scriptsRow);
    m_rexScriptsEdit->setPlaceholderText(tr("None"));
    m_rexScriptsEdit->setClearButtonEnabled(true);
    auto *browse = new QToolButton(scriptsRow);
    browse->setText(tr("Choose…"));
    scriptsLayout->addWidget(m_rexScriptsEdit, 1);
    scriptsLayout->addWidget(browse);
    // On finishing the edit, not every keystroke: each change is sent to
    // every Rex server connected.
    connect(m_rexScriptsEdit, &QLineEdit::editingFinished, this, [this]() {
        if (const QString path = m_rexScriptsEdit->text().trimmed(); path != m_settings.rexScriptsFile())
            m_settings.setRexScriptsFile(path);
    });
    connect(m_rexScriptsEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty() && !m_settings.rexScriptsFile().isEmpty())  // the clear button
            m_settings.setRexScriptsFile(QString());
    });
    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString start = m_rexScriptsEdit->text().isEmpty() ? QDir::homePath() : m_rexScriptsEdit->text();
        const QString path =
            QFileDialog::getOpenFileName(this, tr("Rex Scripts"), start, tr("Lua (*.lua);;All files (*)"));
        if (path.isEmpty())
            return;
        m_rexScriptsEdit->setText(path);
        m_settings.setRexScriptsFile(path);
    });
    rex->addRow(tr("Scripts"), scriptsRow);
    rex->addRow(QString(), makeHint(tr("Key handlers for Rex servers, sent on connecting: "
                                       "rex.bind('ctrl+n', function(ctx, key) … return true end). "
                                       "Choose the file again to resend it after editing."), page));
#endif

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildWindowPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // ---- tabs ------------------------------------------------------------------
    QFormLayout *tabs = addGroup(layout, tr("Tabs"));

    m_tabLayoutBox = new QComboBox(page);
    m_tabLayoutBox->addItem(tr("Tabs along the top"), QStringLiteral("standard"));
    m_tabLayoutBox->addItem(tr("Thumbnail sidebar"), QStringLiteral("thumbnails"));
    connect(m_tabLayoutBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setTabLayout(m_tabLayoutBox->itemData(i).toString());
    });
    tabs->addRow(tr("Layout"), m_tabLayoutBox);

    // ---- watching ------------------------------------------------------------
    QFormLayout *watch = addGroup(layout, tr("Watch a Terminal"));

    m_monitorActivityBox = new QCheckBox(tr("Mark the tab when output arrives"), page);
    connect(m_monitorActivityBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setMonitorActivity(v);
    });
    watch->addRow(tr("Activity"), m_monitorActivityBox);

    // The idle time reads as the end of the checkbox's sentence.
    auto *silenceRow = new QWidget(page);
    auto *silenceLayout = new QHBoxLayout(silenceRow);
    silenceLayout->setContentsMargins(0, 0, 0, 0);
    m_monitorSilenceBox = new QCheckBox(tr("Mark the tab when output stops for"), silenceRow);
    m_silenceTimeoutBox = makeSpinBox<QSpinBox>(silenceRow);
    m_silenceTimeoutBox->setRange(1, 600);
    m_silenceTimeoutBox->setSuffix(tr(" s"));
    silenceLayout->addWidget(m_monitorSilenceBox);
    silenceLayout->addWidget(m_silenceTimeoutBox);
    silenceLayout->addStretch(1);
    connect(m_monitorSilenceBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setMonitorSilence(v);
        m_silenceTimeoutBox->setEnabled(v);
    });
    connect(m_silenceTimeoutBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setSilenceTimeout(v);
    });
    watch->addRow(tr("Silence"), silenceRow);
    watch->addRow(QString(), makeHint(tr("A handy \"build finished\" cue. Marks clear "
                                         "when you select the tab."), page));

    m_notifyProgramStatusBox = new QCheckBox(tr("Notify when a program needs you, finishes or fails"), page);
    connect(m_notifyProgramStatusBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setNotifyProgramStatus(v);
    });
    watch->addRow(tr("Programs"), m_notifyProgramStatusBox);
    m_tintBlockedTabsBox = new QCheckBox(tr("Tint tabs whose program needs you"), page);
    connect(m_tintBlockedTabsBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setTintBlockedTabs(v);
    });
    watch->addRow(QString(), m_tintBlockedTabsBox);
    m_floatBlockedTabsBox = new QCheckBox(tr("Move them to the front until answered"), page);
    connect(m_floatBlockedTabsBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setFloatBlockedTabs(v);
    });
    watch->addRow(QString(), m_floatBlockedTabsBox);
    watch->addRow(QString(), makeHint(tr("For programs that say what they're doing (OSC 7501), "
                                         "such as Claude Code. Notifications are only for tabs "
                                         "you aren't looking at."), page));

    // ---- quitting --------------------------------------------------------------
    QFormLayout *quitting = addGroup(layout, tr("Quitting"));

    m_confirmCloseBox = new QCheckBox(tr("Warn while programs are still running"), page);
    connect(m_confirmCloseBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setConfirmCloseWithRunningProcesses(v);
    });
    quitting->addRow(tr("Confirm"), m_confirmCloseBox);
    quitting->addRow(QString(), makeHint(tr("For an editor, a build or an ssh session. "
                                            "An idle shell prompt never asks."), page));

    m_restoreSessionBox = new QCheckBox(tr("Reopen workspaces and tabs at startup"), page);
    connect(m_restoreSessionBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setRestoreSession(v);
    });
    quitting->addRow(tr("Next start"), m_restoreSessionBox);
    quitting->addRow(QString(), makeHint(tr("Layouts, directories and names; programs "
                                            "aren't restarted. Running tmux sessions "
                                            "are reattached."), page));

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildProfilesPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(12);

    // ---- left: the list + add/duplicate/remove ---------------------------
    auto *left = new QVBoxLayout;
    left->setSpacing(4);

    m_profileList = new QListWidget(page);
    m_profileList->setFixedWidth(190);
    m_profileList->setIconSize(QSize(16, 16));
    m_profileList->setFrameShape(QFrame::StyledPanel);
    left->addWidget(m_profileList, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    auto makeBtn = [page](const QIcon &icon, const QString &fallback, const QString &tip) {
        auto *b = new QToolButton(page);
        b->setIcon(icon);
        b->setText(fallback);   // shown only if the theme has no icon
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_profileAdd = makeBtn(appIcon("plus"), QStringLiteral("+"), tr("New profile"));
    m_profileDuplicate = makeBtn(appIcon("copy"), tr("Copy"),
                                 tr("Duplicate the selected profile"));
    m_profileDelete = makeBtn(appIcon("minus"), QStringLiteral("−"),
                              tr("Delete the selected profile"));
    buttons->addWidget(m_profileAdd);
    buttons->addWidget(m_profileDuplicate);
    buttons->addWidget(m_profileDelete);
    buttons->addStretch(1);
    left->addLayout(buttons);

    outer->addLayout(left);

    // ---- right: the form, at its natural height ---------------------------
    auto *right = new QVBoxLayout;
    m_profileFormBox = new QGroupBox(tr("Profile"), page);
    auto *form = new QFormLayout(m_profileFormBox);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);

    m_profileName = new QLineEdit(m_profileFormBox);
    m_profileName->setPlaceholderText(tr("Profile name"));
    form->addRow(tr("Name"), m_profileName);

    m_profileDefaultBox = new QCheckBox(tr("Open new tabs with this profile"), m_profileFormBox);
    form->addRow(tr("Default"), m_profileDefaultBox);

    form->addRow(QString(), makeHint(tr("Fields left at “Inherit” follow the global "
                                        "settings."), m_profileFormBox));

    m_profileScheme = new QComboBox(m_profileFormBox);
    m_profileScheme->addItem(tr("Inherit"), QString());
    form->addRow(tr("Colour scheme"), m_profileScheme);

    m_profileFont = new QComboBox(m_profileFormBox);
    m_profileFont->addItem(tr("Inherit"), QString());
    for (const QString &f : monoFamilies())
        m_profileFont->addItem(f, f);
    form->addRow(tr("Font"), m_profileFont);

    m_profileFontSize = makeSpinBox<QDoubleSpinBox>(m_profileFormBox);
    m_profileFontSize->setRange(kInheritFontSize, kFontSizeMax);
    m_profileFontSize->setSingleStep(0.5);
    m_profileFontSize->setDecimals(1);
    m_profileFontSize->setSuffix(tr(" pt"));
    m_profileFontSize->setSpecialValueText(tr("Inherit"));
    m_profileFontSize->setValue(kInheritFontSize);
    form->addRow(tr("Font size"), m_profileFontSize);

    m_profileShell = new QLineEdit(m_profileFormBox);
    m_profileShell->setPlaceholderText(tr("Inherit (login shell)"));
    m_profileShell->setToolTip(tr("Full path to a shell or program, e.g. /usr/bin/fish"));
    form->addRow(tr("Shell program"), m_profileShell);

    m_profileShellArgs = new QLineEdit(m_profileFormBox);
    m_profileShellArgs->setToolTip(tr("Space-separated arguments for the shell program"));
    syncShellArgs(m_profileShellArgs, false);
    form->addRow(tr("Arguments"), m_profileShellArgs);

    auto *dirRow = new QWidget(m_profileFormBox);
    auto *dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(0, 0, 0, 0);
    dirLayout->setSpacing(4);
    m_profileWorkDir = new QLineEdit(dirRow);
    m_profileWorkDir->setPlaceholderText(tr("Inherit (home directory)"));
    auto *browse = new QToolButton(dirRow);
    browse->setIcon(appIcon("folder-open"));
    browse->setText(tr("…"));
    browse->setToolTip(tr("Choose a folder"));
    dirLayout->addWidget(m_profileWorkDir, 1);
    dirLayout->addWidget(browse);
    form->addRow(tr("Start in"), dirRow);

    right->addWidget(m_profileFormBox);
    right->addStretch(1);
    outer->addLayout(right, 1);

    /*
     * Wiring.
     *
     * Commit on commit-oriented signals (editingFinished, activated) rather
     * than on every keystroke: renaming a profile should not rewrite the
     * config file once per character, and a half-typed shell path is not a
     * value worth persisting.
     */
    connect(m_profileList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        loadProfileIntoForm(currentProfileId());
    });

    connect(m_profileName, &QLineEdit::editingFinished,
            this, &SettingsDialog::commitProfileForm);
    connect(m_profileShell, &QLineEdit::editingFinished,
            this, &SettingsDialog::commitProfileForm);
    connect(m_profileShellArgs, &QLineEdit::editingFinished,
            this, &SettingsDialog::commitProfileForm);
    connect(m_profileWorkDir, &QLineEdit::editingFinished,
            this, &SettingsDialog::commitProfileForm);
    connect(m_profileScheme, &QComboBox::currentIndexChanged,
            this, [this](int) { commitProfileForm(); });
    connect(m_profileFont, &QComboBox::currentIndexChanged,
            this, [this](int) { commitProfileForm(); });
    connect(m_profileFontSize, &QDoubleSpinBox::valueChanged,
            this, [this](double) { commitProfileForm(); });

    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString start = m_profileWorkDir->text().trimmed().isEmpty()
                                  ? QDir::homePath() : m_profileWorkDir->text().trimmed();
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Start Directory"), start);
        if (dir.isEmpty())
            return;
        m_profileWorkDir->setText(dir);
        commitProfileForm();
    });

    // Only ever ticked: there must always be a default, so it is changed by
    // ticking another profile, never by unticking this one (the box is
    // disabled while it is ticked).
    connect(m_profileDefaultBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loadingProfile || !on)
            return;
        const QString id = currentProfileId();
        if (m_profiles.setDefault(id))
            rebuildProfileList(id);
    });

    connect(m_profileAdd, &QToolButton::clicked, this, [this]() {
        TerminalProfile p;
        // A readable, non-colliding placeholder name, so an immediately-used
        // profile does not look like an error.
        int n = m_profiles.profiles().size() + 1;
        QString name;
        auto taken = [this](const QString &c) {
            for (const auto &e : m_profiles.profiles()) {
                if (e.name.compare(c, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        };
        do {
            name = tr("Profile %1").arg(n++);
        } while (taken(name));
        p.name = name;
        const QString id = m_profiles.add(p);
        rebuildProfileList(id);
        m_profileName->setFocus();
        m_profileName->selectAll();
    });

    connect(m_profileDuplicate, &QToolButton::clicked, this, [this]() {
        const QString id = currentProfileId();
        if (id.isEmpty())
            return;
        const QString copy = m_profiles.duplicate(id);
        if (!copy.isEmpty())
            rebuildProfileList(copy);
    });

    connect(m_profileDelete, &QToolButton::clicked, this, [this]() {
        const QString id = currentProfileId();
        // The button is disabled for the default profile, and the store
        // refuses it too.
        if (id.isEmpty() || id == m_profiles.defaultProfile().id)
            return;
        const auto answer = QMessageBox::question(
            this, tr("Delete Profile"),
            tr("Delete “%1”?\n\nTabs already open with it keep their settings.")
                .arg(m_profiles.profileById(id).name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        m_profiles.remove(id);
        rebuildProfileList();
    });

    reloadProfileSchemeBox();
    rebuildProfileList();
    return page;
}

void SettingsDialog::rebuildProfileList(const QString &selectId)
{
    if (!m_profileList)
        return;

    const QString want = selectId.isEmpty() ? currentProfileId() : selectId;

    // Block signals so repopulating does not fire currentRowChanged for every
    // intermediate row, which would reload (and re-commit) the form repeatedly.
    const QSignalBlocker block(m_profileList);
    m_profileList->clear();
    const QIcon star = appIcon("star");
    const QIcon blank = blankIcon(m_profileList->iconSize().width());
    for (const auto &p : m_profiles.profiles()) {
        auto *item = new QListWidgetItem(p.name);
        item->setData(Qt::UserRole, p.id);
        if (p.isDefault) {
            // A star rather than "(default)" in the name. Without an icon
            // theme, fall back to the old suffix so it is still marked.
            if (star.isNull())
                item->setText(tr("%1 (default)").arg(p.name));
            item->setIcon(star.isNull() ? blank : star);
            item->setToolTip(tr("Default profile: New Tab opens this one"));
        } else {
            item->setIcon(blank);
        }
        m_profileList->addItem(item);
    }

    int row = 0;
    for (int i = 0; i < m_profileList->count(); ++i) {
        if (m_profileList->item(i)->data(Qt::UserRole).toString() == want) {
            row = i;
            break;
        }
    }
    m_profileList->setCurrentRow(row);

    loadProfileIntoForm(currentProfileId());
    setProfileFormEnabled(m_profileList->count() > 0);
}

QString SettingsDialog::currentProfileId() const
{
    if (!m_profileList || !m_profileList->currentItem())
        return QString();
    return m_profileList->currentItem()->data(Qt::UserRole).toString();
}

void SettingsDialog::setProfileFormEnabled(bool on)
{
    if (m_profileFormBox)
        m_profileFormBox->setEnabled(on);
}

void SettingsDialog::loadProfileIntoForm(const QString &id)
{
    if (!m_profileFormBox || id.isEmpty())
        return;

    const TerminalProfile p = m_profiles.profileById(id);

    // Guard every setter below: each one would otherwise look like a user edit
    // and be written straight back.
    m_loadingProfile = true;

    m_profileName->setText(p.name);

    // The default can't be unticked (there must always be one), only moved by
    // ticking another profile; and it can't be deleted.
    m_profileDefaultBox->setChecked(p.isDefault);
    m_profileDefaultBox->setEnabled(!p.isDefault);
    m_profileDefaultBox->setToolTip(
        p.isDefault ? tr("To change the default, tick this on another profile")
                    : QString());
    m_profileDelete->setEnabled(!p.isDefault);
    m_profileDelete->setToolTip(p.isDefault
                                    ? tr("The default profile can't be deleted")
                                    : tr("Delete the selected profile"));

    // Inherit is index 0; a value no longer in the list (e.g. a scheme that has
    // been removed) falls back to Inherit rather than being silently rewritten.
    m_profileScheme->setCurrentIndex(
        p.colorScheme ? qMax(0, m_profileScheme->findData(*p.colorScheme)) : 0);
    m_profileFont->setCurrentIndex(
        p.fontFamily ? qMax(0, m_profileFont->findData(*p.fontFamily)) : 0);

    m_profileFontSize->setValue(p.fontSize ? *p.fontSize : kInheritFontSize);

    m_profileShell->setText(p.shellProgram ? *p.shellProgram : QString());
    m_profileShellArgs->setText(
        (p.shellProgram && p.shellArgs) ? p.shellArgs->join(QLatin1Char(' '))
                                        : QString());
    m_profileWorkDir->setText(p.workingDirectory ? *p.workingDirectory
                                                 : QString());
    syncShellArgs(m_profileShellArgs, p.shellProgram.has_value());

    m_loadingProfile = false;
}

void SettingsDialog::commitProfileForm()
{
    if (m_loadingProfile || !m_profileFormBox)
        return;
    const QString id = currentProfileId();
    if (id.isEmpty())
        return;

    TerminalProfile p = m_profiles.profileById(id);

    const QString name = m_profileName->text().trimmed();
    if (!name.isEmpty())
        p.name = name;

    // Inherit -> nullopt; a real value -> that value.
    const QString scheme = m_profileScheme->currentData().toString();
    p.colorScheme = scheme.isEmpty() ? std::nullopt
                                     : std::optional<QString>(scheme);
    const QString family = m_profileFont->currentData().toString();
    p.fontFamily = family.isEmpty() ? std::nullopt
                                    : std::optional<QString>(family);

    if (m_profileFontSize->value() <= kInheritFontSize)
        p.fontSize = std::nullopt;
    else
        p.fontSize = m_profileFontSize->value();

    const QString shell = m_profileShell->text().trimmed();
    if (shell.isEmpty()) {
        p.shellProgram = std::nullopt;
        p.shellArgs = std::nullopt;
    } else {
        p.shellProgram = shell;
        // An empty list is meaningful ("this program, no arguments"), so keep
        // it rather than leaving args unset.
        const QString argsText = m_profileShellArgs->text().trimmed();
        p.shellArgs = argsText.isEmpty()
                          ? QStringList()
                          : argsText.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
    syncShellArgs(m_profileShellArgs, !shell.isEmpty());

    const QString dir = m_profileWorkDir->text().trimmed();
    p.workingDirectory = dir.isEmpty() ? std::nullopt
                                       : std::optional<QString>(dir);

    const bool renamed = p.name != m_profiles.profileById(id).name;
    m_profiles.update(p);
    // The list shows names, so a rename has to reach it.
    if (renamed)
        rebuildProfileList(id);
}

void SettingsDialog::editColorScheme()
{
    const QString seed = m_schemeBox->currentText();
    ColorSchemeEditor editor(seed, this);
    if (editor.exec() != QDialog::Accepted)
        return;

    const QString name = editor.savedName();
    const QString path = editor.savedPath();
    if (name.isEmpty() || path.isEmpty())
        return;

    /*
     * The scheme manager caches its list the first time it is asked, so a
     * scheme written during this run is not in availableColorSchemes(). Load it
     * explicitly so applying it by path below finds a real table rather than
     * silently falling back to the default scheme.
     */
    ColorSchemeFile().load(path);

    reloadSchemeBox(name);

    /*
     * Select the scheme. The combo drives Settings::setColorScheme, which
     * TerminalWidget::applyAppearance() passes to QTermWidget::setColorScheme.
     *
     * A bare name is only resolved when the manager already knows it; an
     * unknown name silently falls back to the DEFAULT scheme with no error, so
     * applying by full path is the only reliable option here.
     */
    const QString dir = ColorSchemeFile::userSchemeDir();
    if (!dir.isEmpty())
        QTermWidget::addCustomColorSchemeDir(dir);

    /*
     * An overwrite needs special handling. qtermwidget has the scheme cached
     * under this basename and cannot reload it: the freshly written file is
     * ignored (loadColorScheme() deletes the new parse, findColorScheme()
     * returns the stale instance), so simply re-applying it would leave the
     * terminal showing the OLD colours.
     *
     * Applying a throwaway copy with a fresh basename bypasses the cache. The
     * live path is then what the session uses; Settings remembers the real name
     * so the next start resolves back to the user's own scheme (see
     * Settings::finaliseLiveSchemes()).
     */
    if (editor.overwroteExisting()) {
        ColorSchemeFile live;
        if (live.load(path)) {
            const QString livePath = live.saveLiveCopy();
            if (!livePath.isEmpty()) {
                m_settings.setLiveScheme(name, livePath);
                return;
            }
        }
        // Fall through to the plain apply if the live copy could not be made.
    }

    m_settings.setColorScheme(path);
}

void SettingsDialog::reloadSchemeBox(const QString &selectName)
{
    if (!m_schemeBox)
        return;

    // Rebuild rather than append: the order should be stable, and a
    // deleted/renamed scheme must not linger. allSchemeNames() merges the
    // user's saved schemes, which the cached library list omits.
    const QSignalBlocker block(m_schemeBox);
    m_schemeBox->clear();
    m_schemeBox->addItems(ColorSchemeFile::allSchemeNames());

    // Prefer the requested scheme; fall back to whatever is currently active.
    QString target = selectName;
    if (target.isEmpty())
        target = m_settings.appearance().colorScheme;
    // Settings may hold a full path: either a scheme the editor saved (the
    // combo lists bare names) or a live stand-in for an in-place edit, whose
    // basename is a throwaway. For a live path the real scheme is the mapping's
    // other half, so ask Settings for that instead.
    if (m_settings.isLiveScheme(target))
        target = m_settings.liveSchemeRealName();
    if (target.contains(QLatin1Char('/')))
        target = QFileInfo(target).baseName();

    const int index = m_schemeBox->findText(target, Qt::MatchFixedString);
    if (index >= 0)
        m_schemeBox->setCurrentIndex(index);

    // The profile form lists the same schemes, so it needs the new one too.
    reloadProfileSchemeBox();
}

void SettingsDialog::reloadProfileSchemeBox(const QString &selectName)
{
    if (!m_profileScheme)
        return;

    // Blocked: the combo's currentIndexChanged handler commits the profile, so
    // repopulating it must not look like a user edit.
    const QSignalBlocker block(m_profileScheme);

    QString target = selectName;
    if (target.isEmpty())
        target = m_profileScheme->currentData().toString();
    // A profile may hold a full path if it was set by hand; the list is names.
    if (target.contains(QLatin1Char('/')))
        target = QFileInfo(target).baseName();

    m_profileScheme->clear();
    m_profileScheme->addItem(tr("Inherit"), QString());
    for (const QString &s : ColorSchemeFile::allSchemeNames())
        m_profileScheme->addItem(s, s);

    int index = m_profileScheme->findData(target);
    if (index < 0)
        index = 0;   // Keep the selection visible; the profile is unchanged.
    m_profileScheme->setCurrentIndex(index);
}

void SettingsDialog::syncFromSettings()
{
    const auto &a = m_settings.appearance();
    const auto &s = m_settings.scrolling();

    // These setters fire the change handlers, which write back to Settings.
    // Writing back the value Settings already holds is a no-op, so the round
    // trip is harmless.
    m_schemeBox->setCurrentText(a.colorScheme);
    const int pct = qRound(a.opacity * 100);
    m_opacitySlider->setValue(pct);
    m_opacityLabel->setText(tr("%1%").arg(pct));

    if (const int fi = m_fontBox->findText(a.fontFamily); fi >= 0)
        m_fontBox->setCurrentIndex(fi);
    m_fontSizeBox->setValue(a.fontSize);
    m_lineSpacingBox->setValue(a.lineSpacing);
    m_marginBox->setValue(a.terminalMargin);

    m_boldBox->setChecked(a.boldIntense);
    m_italicBox->setChecked(a.italic);
    m_antialiasBox->setChecked(a.antialias);
    m_lineCharsBox->setChecked(a.drawLineChars);
    m_bidiBox->setChecked(a.bidi);
    m_paneCardsBox->setChecked(a.paneCards);

    const int cursorIndex = m_cursorShapeBox->findData(QString::number(a.cursorShape));
    if (cursorIndex >= 0)
        m_cursorShapeBox->setCurrentIndex(cursorIndex);
    m_blinkBox->setChecked(a.blinkingCursor);

    m_scrollbackBox->setValue(s.scrollback);
    const int sbIndex = m_scrollbarPosBox->findData(s.scrollbarPosition);
    if (sbIndex >= 0)
        m_scrollbarPosBox->setCurrentIndex(sbIndex);
    m_pasteConfirmBox->setChecked(s.confirmMultilinePaste);
    m_pasteTrimBox->setChecked(s.trimPastedTrailingNewlines);
    if (m_rexScriptsEdit)
        m_rexScriptsEdit->setText(m_settings.rexScriptsFile());
    m_flowControlWarnBox->setChecked(s.flowControlWarning);

    const int layoutIndex = m_tabLayoutBox->findData(a.tabLayout);
    if (layoutIndex >= 0)
        m_tabLayoutBox->setCurrentIndex(layoutIndex);
    m_monitorActivityBox->setChecked(s.monitorActivity);
    m_notifyProgramStatusBox->setChecked(s.notifyProgramStatus);
    m_tintBlockedTabsBox->setChecked(s.tintBlockedTabs);
    m_floatBlockedTabsBox->setChecked(s.floatBlockedTabs);
    m_monitorSilenceBox->setChecked(s.monitorSilence);
    m_silenceTimeoutBox->setValue(s.silenceTimeout);
    m_silenceTimeoutBox->setEnabled(s.monitorSilence);
    m_confirmCloseBox->setChecked(s.confirmCloseWithRunningProcesses);
    m_restoreSessionBox->setChecked(s.restoreSession);
}

// ---- Servers ------------------------------------------------------------------

namespace {
QIcon serverIcon(SavedServer::Kind kind)
{
    switch (kind) {
    case SavedServer::Rex: return appIcon("server");
    case SavedServer::Tmux: return appIcon("square-terminal");
    case SavedServer::Vnc:
    case SavedServer::Rdp: return appIcon("app-window");
    case SavedServer::Web: return appIcon("code-xml");
    }
    return {};
}
} // namespace

QWidget *SettingsDialog::buildServersPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(12);

    // ---- left: the list + add/remove/reorder -------------------------------
    auto *left = new QVBoxLayout;
    left->setSpacing(4);
    m_serverList = new QListWidget(page);
    m_serverList->setFixedWidth(190);
    m_serverList->setIconSize(QSize(16, 16));
    m_serverList->setFrameShape(QFrame::StyledPanel);
    left->addWidget(m_serverList, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    auto makeBtn = [page](const QIcon &icon, const QString &fallback, const QString &tip) {
        auto *b = new QToolButton(page);
        b->setIcon(icon);
        b->setText(fallback);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto *add = makeBtn(appIcon("plus"), QStringLiteral("+"), tr("New server"));
    m_serverRemove = makeBtn(appIcon("minus"), QStringLiteral("\u2212"), tr("Delete the selected server"));
    m_serverUp = makeBtn(QIcon(), QStringLiteral("\u2191"), tr("Move up the list"));
    m_serverDown = makeBtn(QIcon(), QStringLiteral("\u2193"), tr("Move down the list"));
    buttons->addWidget(add);
    buttons->addWidget(m_serverRemove);
    buttons->addStretch(1);
    buttons->addWidget(m_serverUp);
    buttons->addWidget(m_serverDown);
    left->addLayout(buttons);
    outer->addLayout(left);

    // ---- right: the form ------------------------------------------------------
    auto *right = new QVBoxLayout;
    m_serverFormBox = new QGroupBox(tr("Server"), page);
    m_serverForm = new QFormLayout(m_serverFormBox);
    m_serverForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_serverForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_serverForm->setHorizontalSpacing(12);
    m_serverForm->setVerticalSpacing(8);

    m_serverKind = new QComboBox(m_serverFormBox);
    for (SavedServer::Kind k : {SavedServer::Rex, SavedServer::Tmux, SavedServer::Vnc, SavedServer::Rdp,
                                SavedServer::Web})
        m_serverKind->addItem(serverIcon(k), SavedServer::kindName(k), int(k));
    m_serverForm->addRow(tr("Kind"), m_serverKind);

    m_serverName = new QLineEdit(m_serverFormBox);
    m_serverName->setPlaceholderText(tr("As the menu shows it (the address if empty)"));
    m_serverForm->addRow(tr("Name"), m_serverName);

    m_serverAddress = new QLineEdit(m_serverFormBox);
    m_serverForm->addRow(tr("Address"), m_serverAddress);
    m_serverHint = makeHint(QString(), m_serverFormBox);
    m_serverForm->addRow(QString(), m_serverHint);

    m_serverSession = new QLineEdit(m_serverFormBox);
    m_serverSession->setPlaceholderText(QStringLiteral("main"));
    m_serverForm->addRow(tr("Session"), m_serverSession);

    m_serverUser = new QLineEdit(m_serverFormBox);
    m_serverUser->setPlaceholderText(tr("Asked for when needed"));
    m_serverForm->addRow(tr("User"), m_serverUser);

    m_serverReducedColour = new QCheckBox(tr("Fewer colours (faster on slow links)"), m_serverFormBox);
    m_serverForm->addRow(tr("Colour"), m_serverReducedColour);
    m_serverAdaptive = new QCheckBox(tr("Lower quality while the screen is busy"), m_serverFormBox);
    m_serverForm->addRow(tr("Quality"), m_serverAdaptive);

    right->addWidget(m_serverFormBox);
    right->addWidget(makeHint(tr("Saved servers are in the Servers menu. Passwords are never "
                                 "saved: remote desktops ask when they connect."), page));
    right->addStretch(1);
    outer->addLayout(right, 1);

    // ---- wiring ---------------------------------------------------------------
    connect(m_serverList, &QListWidget::currentRowChanged, this, [this](int) {
        auto *item = m_serverList->currentItem();
        loadServerIntoForm(item ? item->data(Qt::UserRole).toString() : QString());
    });
    connect(add, &QToolButton::clicked, this, [this]() {
        SavedServer s;
        s.kind = SavedServer::Rex;
        s.name = tr("New server");
        s.address = QStringLiteral("http://127.0.0.1:51433");
        rebuildServerList(m_servers->save(s));
        m_serverName->setFocus();
        m_serverName->selectAll();
    });
    connect(m_serverRemove, &QToolButton::clicked, this, [this]() {
        auto *item = m_serverList->currentItem();
        if (!item)
            return;
        const int row = m_serverList->currentRow();
        m_servers->remove(item->data(Qt::UserRole).toString());
        rebuildServerList();
        m_serverList->setCurrentRow(qMin(row, m_serverList->count() - 1));
    });
    auto mover = [this](int step) {
        auto *item = m_serverList->currentItem();
        if (!item)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        m_servers->move(id, step);
        rebuildServerList(id);
    };
    connect(m_serverUp, &QToolButton::clicked, this, [mover]() { mover(-1); });
    connect(m_serverDown, &QToolButton::clicked, this, [mover]() { mover(+1); });
    connect(m_serverKind, &QComboBox::activated, this, [this]() {
        updateServerFields();
        commitServerForm();
    });
    for (QLineEdit *e : {m_serverName, m_serverAddress, m_serverSession, m_serverUser})
        connect(e, &QLineEdit::editingFinished, this, &SettingsDialog::commitServerForm);
    for (QCheckBox *c : {m_serverReducedColour, m_serverAdaptive})
        connect(c, &QCheckBox::toggled, this, &SettingsDialog::commitServerForm);

    rebuildServerList();
    if (m_serverList->count())
        m_serverList->setCurrentRow(0);
    else
        loadServerIntoForm(QString());
    return page;
}

void SettingsDialog::rebuildServerList(const QString &selectId)
{
    const QSignalBlocker block(m_serverList);
    m_serverList->clear();
    int select = -1;
    for (const SavedServer &s : m_servers->servers()) {
        auto *item = new QListWidgetItem(serverIcon(s.kind), s.title(), m_serverList);
        item->setData(Qt::UserRole, s.id);
        item->setToolTip(SavedServer::kindName(s.kind) + QStringLiteral(" \u00b7 ") + s.address);
        if (s.id == selectId)
            select = m_serverList->count() - 1;
    }
    if (select >= 0) {
        m_serverList->setCurrentRow(select);
        loadServerIntoForm(selectId);
    }
}

void SettingsDialog::loadServerIntoForm(const QString &id)
{
    m_loadingServer = true;
    const SavedServer s = m_servers->server(id);
    const bool have = !id.isEmpty() && s.id == id;
    m_serverFormBox->setEnabled(have);
    m_serverRemove->setEnabled(have);
    m_serverUp->setEnabled(have && m_serverList->currentRow() > 0);
    m_serverDown->setEnabled(have && m_serverList->currentRow() < m_serverList->count() - 1);
    m_serverKind->setCurrentIndex(m_serverKind->findData(int(s.kind)));
    m_serverName->setText(have ? s.name : QString());
    m_serverAddress->setText(have ? s.address : QString());
    m_serverSession->setText(s.session);
    m_serverUser->setText(s.user);
    m_serverReducedColour->setChecked(s.reducedColour);
    m_serverAdaptive->setChecked(s.adaptive);
    updateServerFields();
    m_loadingServer = false;
}

void SettingsDialog::updateServerFields()
{
    // Only the fields this kind has.
    const auto kind = SavedServer::Kind(m_serverKind->currentData().toInt());
    m_serverForm->setRowVisible(m_serverSession, kind == SavedServer::Tmux);
    m_serverForm->setRowVisible(m_serverUser, kind == SavedServer::Vnc || kind == SavedServer::Rdp);
    m_serverForm->setRowVisible(m_serverReducedColour, kind == SavedServer::Vnc);
    m_serverForm->setRowVisible(m_serverAdaptive, kind == SavedServer::Vnc);
    switch (kind) {
    case SavedServer::Rex:
        m_serverAddress->setPlaceholderText(QStringLiteral("http://127.0.0.1:51433"));
        m_serverHint->setText(tr("A Rex server. Connecting lists its sessions, or starts a new one."));
        break;
    case SavedServer::Tmux:
        m_serverAddress->setPlaceholderText(tr("user@host"));
        m_serverHint->setText(tr("Connects with ssh, which must log in without a password prompt "
                                 "(a key or an agent). The session is created if it doesn't exist."));
        break;
    case SavedServer::Vnc:
        m_serverAddress->setPlaceholderText(QStringLiteral("host:5900"));
        m_serverHint->setText(tr("host, host:port or host:display."));
        break;
    case SavedServer::Rdp:
        m_serverAddress->setPlaceholderText(QStringLiteral("host:3389"));
        m_serverHint->setText(tr("host or host:port."));
        break;
    case SavedServer::Web:
        m_serverAddress->setPlaceholderText(QStringLiteral("localhost:3000"));
        m_serverHint->setText(tr("A page to open in a web tab."));
        break;
    }
}

void SettingsDialog::commitServerForm()
{
    if (m_loadingServer)
        return;
    auto *item = m_serverList->currentItem();
    if (!item)
        return;
    SavedServer s = m_servers->server(item->data(Qt::UserRole).toString());
    if (s.id.isEmpty())
        return;
    s.kind = SavedServer::Kind(m_serverKind->currentData().toInt());
    s.name = m_serverName->text().trimmed();
    s.address = m_serverAddress->text().trimmed();
    s.session = s.kind == SavedServer::Tmux ? m_serverSession->text().trimmed() : QString();
    s.user = (s.kind == SavedServer::Vnc || s.kind == SavedServer::Rdp) ? m_serverUser->text().trimmed() : QString();
    s.reducedColour = s.kind == SavedServer::Vnc && m_serverReducedColour->isChecked();
    s.adaptive = s.kind == SavedServer::Vnc && m_serverAdaptive->isChecked();
    m_servers->save(s);
    item->setText(s.title());
    item->setIcon(serverIcon(s.kind));
    item->setToolTip(SavedServer::kindName(s.kind) + QStringLiteral(" \u00b7 ") + s.address);
}
