/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settingsdialog.h"

#include "colorscheme.h"
#include "colorschemeeditor.h"
#include "profiles.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileInfo>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <qtermwidget.h>

namespace {
// Half-point steps, matching the QML build.
constexpr int kFontSliderMin = 8;
constexpr int kFontSliderMax = 32;

QString formatPoints(double pt)
{
    const double v = qRound(pt * 2) / 2.0;
    return (qFuzzyCompare(v, qRound(v)) ? QString::number(qRound(v))
                                        : QString::number(v)) + QStringLiteral("pt");
}

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
} // namespace

SettingsDialog::SettingsDialog(Settings &settings, ProfileStore &profiles,
                               QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_profiles(profiles)
{
    setWindowTitle(tr("Settings"));
    setModal(true);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_sections = new QListWidget(this);
    m_sections->setFixedWidth(170);
    m_sections->addItems({tr("Appearance"), tr("Text"), tr("Cursor"),
                          tr("Scrolling"), tr("Profiles")});
    m_sections->setFrameShape(QFrame::NoFrame);
    outer->addWidget(m_sections);

    auto *right = new QVBoxLayout;
    right->setContentsMargins(16, 16, 16, 12);
    right->setSpacing(12);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildAppearancePage());
    m_pages->addWidget(buildTextPage());
    m_pages->addWidget(buildCursorPage());
    m_pages->addWidget(buildScrollingPage());
    m_pages->addWidget(buildProfilesPage());
    right->addWidget(m_pages, 1);

    auto *footer = new QHBoxLayout;
    auto *hint = new QLabel(tr("Changes apply immediately and are remembered."), this);
    hint->setEnabled(false);
    footer->addWidget(hint, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    footer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    right->addLayout(footer);

    outer->addLayout(right, 1);

    connect(m_sections, &QListWidget::currentRowChanged,
            m_pages, &QStackedWidget::setCurrentIndex);

    m_sections->setCurrentRow(Appearance);
    syncFromSettings();

    // Size to the tallest page rather than a hard-coded pixel count: with the
    // monitoring group added, the Scrolling page's natural height (~500px on
    // KDE fonts) used to exceed the old fixed 470px and squeeze the group
    // box's contents to nothing on some styles. This keeps every control
    // visible while staying as compact as the content allows.
    //
    // adjustSize() is not enough on its own: Qt caps it at two-thirds of the
    // screen, so on a short screen the Scrolling page (which is the tallest)
    // gets clipped and its group boxes collapse to a few pixels. Ask for the
    // page's real height as a floor.
    adjustSize();
    const int contentHeight = m_pages->sizeHint().height()
                              + (height() - m_pages->height());  // + footer
    if (height() < contentHeight)
        resize(width(), contentHeight);
    if (width() < 680 || height() < 470)
        resize(qMax(width(), 680), qMax(height(), 470));
}

void SettingsDialog::showSection(Section section)
{
    m_sections->setCurrentRow(int(section));
    show();
    raise();
    activateWindow();
}

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);

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
    m_editSchemeBtn = new QPushButton(tr("&Edit…"), schemeRow);
    m_editSchemeBtn->setToolTip(tr("Create a new scheme from a copy of this one"));
    schemeLayout->addWidget(m_editSchemeBtn);
    form->addRow(tr("Colour scheme"), schemeRow);

    connect(m_editSchemeBtn, &QPushButton::clicked, this,
            &SettingsDialog::editColorScheme);

    auto *schemeHint = new QLabel(
        tr("“Edit…” always starts from a copy, so the selected scheme is never "
           "modified. Saved schemes appear in this list."), page);
    schemeHint->setWordWrap(true);
    schemeHint->setEnabled(false);
    form->addRow(QString(), schemeHint);

    m_tabLayoutBox = new QComboBox(page);
    m_tabLayoutBox->addItem(tr("Tabs along the top"), QStringLiteral("standard"));
    m_tabLayoutBox->addItem(tr("Thumbnail sidebar"), QStringLiteral("thumbnails"));
    connect(m_tabLayoutBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setTabLayout(m_tabLayoutBox->itemData(i).toString());
    });
    form->addRow(tr("Tab layout"), m_tabLayoutBox);

    m_marginBox = new QSpinBox(page);
    m_marginBox->setRange(0, 32);
    m_marginBox->setSuffix(QStringLiteral(" px"));
    connect(m_marginBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setTerminalMargin(v);
    });
    form->addRow(tr("Terminal margin"), m_marginBox);

    auto *marginHint = new QLabel(tr("Empty space painted around the terminal grid, "
                                     "so the text does not touch the window edges."), page);
    marginHint->setWordWrap(true);
    marginHint->setEnabled(false);
    form->addRow(QString(), marginHint);

    m_bidiBox = new QCheckBox(tr("Bidirectional text (for right-to-left scripts)"), page);
    connect(m_bidiBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBidi(v);
    });
    form->addRow(QString(), m_bidiBox);

    return page;
}

QWidget *SettingsDialog::buildTextPage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);

    m_fontBox = new QComboBox(page);
    m_fontBox->addItems(monoFamilies());
    connect(m_fontBox, &QComboBox::currentTextChanged, this, [this](const QString &v) {
        m_settings.setFontFamily(v);
    });
    form->addRow(tr("Font"), m_fontBox);

    auto *hint = new QLabel(tr("Only fixed-pitch families are listed, since the "
                               "terminal cell grid depends on them."), page);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    form->addRow(QString(), hint);

    auto *sizeRow = new QWidget(page);
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    m_fontSizeSlider = new QSlider(Qt::Horizontal, sizeRow);
    m_fontSizeSlider->setRange(kFontSliderMin * 2, kFontSliderMax * 2); // half-points
    m_fontSizeSlider->setSingleStep(1);
    m_fontSizeSlider->setPageStep(2);
    m_fontSizeLabel = new QLabel(sizeRow);
    m_fontSizeLabel->setMinimumWidth(52);
    m_fontSizeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sizeLayout->addWidget(m_fontSizeSlider, 1);
    sizeLayout->addWidget(m_fontSizeLabel);
    connect(m_fontSizeSlider, &QSlider::valueChanged, this, [this](int half) {
        const double pt = half / 2.0;
        m_fontSizeLabel->setText(formatPoints(pt));
        m_settings.setFontSize(pt);
    });
    form->addRow(tr("Size"), sizeRow);

    // Background opacity as a 30–100% slider; matching the Settings range so
    // the text is always at least faintly visible. Revives the QML build's
    // appearance/opacity key, which the widgets API can serve after all.
    auto *opRow = new QWidget(page);
    auto *opLayout = new QHBoxLayout(opRow);
    opLayout->setContentsMargins(0, 0, 0, 0);
    m_opacitySlider = new QSlider(Qt::Horizontal, opRow);
    m_opacitySlider->setRange(30, 100);
    m_opacitySlider->setSingleStep(5);
    m_opacitySlider->setPageStep(10);
    m_opacityLabel = new QLabel(opRow);
    m_opacityLabel->setMinimumWidth(52);
    m_opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opLayout->addWidget(m_opacitySlider, 1);
    opLayout->addWidget(m_opacityLabel);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int pct) {
        m_opacityLabel->setText(tr("%1%").arg(pct));
        m_settings.setOpacity(pct / 100.0);
    });
    form->addRow(tr("Opacity"), opRow);

    // No line-spacing control here on purpose: QTermWidget has no setter for
    // it, so a control would do nothing. The setting is still read and
    // preserved so the shared config file stays compatible with the QML build.

    auto *rendering = new QGroupBox(tr("Rendering"), page);
    auto *rLayout = new QVBoxLayout(rendering);
    m_boldBox = new QCheckBox(tr("Bold intense text"), rendering);
    connect(m_boldBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBoldIntense(v);
    });
    m_italicBox = new QCheckBox(tr("Italic"), rendering);
    connect(m_italicBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setItalic(v);
    });
    m_antialiasBox = new QCheckBox(tr("Antialias text"), rendering);
    connect(m_antialiasBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setAntialias(v);
    });
    rLayout->addWidget(m_boldBox);
    rLayout->addWidget(m_italicBox);
    rLayout->addWidget(m_antialiasBox);
    form->addRow(rendering);

    return page;
}

QWidget *SettingsDialog::buildCursorPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *shapeRow = new QHBoxLayout;
    shapeRow->addWidget(new QLabel(tr("Cursor shape"), page));
    m_cursorShapeBox = new QComboBox(page);
    m_cursorShapeBox->addItem(tr("Block"),     QStringLiteral("0"));
    m_cursorShapeBox->addItem(tr("Underline"), QStringLiteral("1"));
    m_cursorShapeBox->addItem(tr("I-Beam"),    QStringLiteral("2"));
    connect(m_cursorShapeBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setCursorShape(m_cursorShapeBox->itemData(i).toInt());
    });
    shapeRow->addWidget(m_cursorShapeBox, 1);
    layout->addLayout(shapeRow);

    m_blinkBox = new QCheckBox(tr("Blinking cursor"), page);
    connect(m_blinkBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setBlinkingCursor(v);
    });
    layout->addWidget(m_blinkBox);

    auto *hint = new QLabel(tr("The cursor only blinks while the terminal holds "
                               "keyboard focus."), page);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);
    layout->addStretch(1);

    return page;
}

QWidget *SettingsDialog::buildScrollingPage()
{
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);

    m_scrollbackBox = new QSpinBox(page);
    m_scrollbackBox->setRange(0, 200000);
    m_scrollbackBox->setSingleStep(500);
    connect(m_scrollbackBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setScrollback(v);
    });
    form->addRow(tr("Scrollback (lines)"), m_scrollbackBox);

    auto *scrollHint = new QLabel(tr("Applied to open tabs immediately. This is "
                                     "something the QML build could not do."), page);
    scrollHint->setWordWrap(true);
    scrollHint->setEnabled(false);
    form->addRow(QString(), scrollHint);

    m_scrollbarPosBox = new QComboBox(page);
    m_scrollbarPosBox->addItem(tr("None"),  QStringLiteral("none"));
    m_scrollbarPosBox->addItem(tr("Left"),  QStringLiteral("left"));
    m_scrollbarPosBox->addItem(tr("Right"), QStringLiteral("right"));
    connect(m_scrollbarPosBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setScrollbarPosition(m_scrollbarPosBox->itemData(i).toString());
    });
    form->addRow(tr("Scrollbar position"), m_scrollbarPosBox);

    m_flowControlWarnBox = new QCheckBox(tr("Warn when flow control freezes the display"), page);
    connect(m_flowControlWarnBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setFlowControlWarning(v);
    });
    form->addRow(QString(), m_flowControlWarnBox);

    auto *flowHint = new QLabel(tr("When a program holds XOFF (e.g. after Ctrl-S the "
                                   "output pauses), show a notice instead of looking "
                                   "hung. Applies live to open tabs."), page);
    flowHint->setWordWrap(true);
    flowHint->setEnabled(false);
    form->addRow(QString(), flowHint);

    // ---- closing ----------------------------------------------------------
    m_confirmCloseBox = new QCheckBox(tr("Warn before quitting with programs running"), page);
    connect(m_confirmCloseBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setConfirmCloseWithRunningProcesses(v);
    });
    form->addRow(QString(), m_confirmCloseBox);

    auto *closeHint = new QLabel(tr("Asks for confirmation when a terminal is running a "
                                    "foreground program (an editor, a build, an ssh "
                                    "session) that quitting would terminate. An idle "
                                    "shell prompt never triggers it."), page);
    closeHint->setWordWrap(true);
    closeHint->setEnabled(false);
    form->addRow(QString(), closeHint);

    // ---- output monitoring ------------------------------------------------
    auto *watchGroup = new QGroupBox(tr("Watch a terminal"), page);
    auto *watchLayout = new QVBoxLayout(watchGroup);

    m_monitorActivityBox = new QCheckBox(tr("Mark the tab when output arrives"), watchGroup);
    connect(m_monitorActivityBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setMonitorActivity(v);
    });
    watchLayout->addWidget(m_monitorActivityBox);

    m_monitorSilenceBox = new QCheckBox(tr("Mark the tab when output stops"), watchGroup);
    connect(m_monitorSilenceBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setMonitorSilence(v);
    });
    watchLayout->addWidget(m_monitorSilenceBox);

    auto *timeoutRow = new QHBoxLayout;
    auto *timeoutLabel = new QLabel(tr("Idle for"), watchGroup);
    m_silenceTimeoutBox = new QSpinBox(watchGroup);
    m_silenceTimeoutBox->setRange(1, 600);
    m_silenceTimeoutBox->setSuffix(QStringLiteral(" s"));
    connect(m_silenceTimeoutBox, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.setSilenceTimeout(v);
    });
    timeoutRow->addSpacing(20); // indent under the checkbox above
    timeoutRow->addWidget(timeoutLabel);
    timeoutRow->addWidget(m_silenceTimeoutBox);
    timeoutRow->addStretch(1);
    watchLayout->addLayout(timeoutRow);

    auto *watchHint = new QLabel(tr("Silence means \"no output for this many "
                                    "seconds\" — a useful \"build finished\" cue. "
                                    "Both markers clear when you select the tab."), watchGroup);
    watchHint->setWordWrap(true);
    watchHint->setEnabled(false);
    watchLayout->addWidget(watchHint);

    // The timeout row is only meaningful while silence monitoring is on.
    auto syncTimeoutEnabled = [this]() {
        m_silenceTimeoutBox->setEnabled(m_monitorSilenceBox->isChecked());
    };
    connect(m_monitorSilenceBox, &QCheckBox::toggled,
            this, [syncTimeoutEnabled](bool) { syncTimeoutEnabled(); });

    form->addRow(watchGroup);

    m_lineCharsBox = new QCheckBox(tr("Terminal draws its own box characters"), page);
    connect(m_lineCharsBox, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.setDrawLineChars(v);
    });
    form->addRow(QString(), m_lineCharsBox);

    auto *lineHint = new QLabel(tr("On by default. Box frames and block art are "
                                   "painted as vectors rather than taken from the "
                                   "font, so they stay crisp at any point size. "
                                   "Turn this off to use the font's own glyphs."), page);
    lineHint->setWordWrap(true);
    lineHint->setEnabled(false);
    form->addRow(QString(), lineHint);

    return page;
}

QWidget *SettingsDialog::buildProfilesPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(12);

    // ---- left: the list + CRUD ------------------------------------------
    auto *left = new QVBoxLayout;
    left->setSpacing(6);

    m_profileList = new QListWidget(page);
    m_profileList->setMinimumWidth(170);
    m_profileList->setFrameShape(QFrame::StyledPanel);
    left->addWidget(m_profileList, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(4);
    auto makeBtn = [page](const QString &text, const QString &tip) {
        auto *b = new QPushButton(text, page);
        b->setToolTip(tip);
        return b;
    };
    m_profileAdd = makeBtn(tr("Add"), tr("Create a new profile"));
    m_profileDuplicate = makeBtn(tr("Duplicate"), tr("Copy the selected profile"));
    m_profileDelete = makeBtn(tr("Delete"), tr("Delete the selected profile"));
    buttons->addWidget(m_profileAdd);
    buttons->addWidget(m_profileDuplicate);
    buttons->addWidget(m_profileDelete);
    left->addLayout(buttons);

    outer->addLayout(left);

    // ---- right: the form -------------------------------------------------
    m_profileFormBox = new QGroupBox(tr("Profile settings"), page);
    auto *form = new QFormLayout(m_profileFormBox);
    form->setSpacing(8);

    m_profileName = new QLineEdit(m_profileFormBox);
    m_profileName->setPlaceholderText(tr("Profile name"));
    form->addRow(tr("Name"), m_profileName);

    auto *inheritHint = new QLabel(
        tr("Leave a field at “Inherit” to follow the global setting."),
        m_profileFormBox);
    inheritHint->setWordWrap(true);
    inheritHint->setEnabled(false);
    form->addRow(QString(), inheritHint);

    m_profileScheme = new QComboBox(m_profileFormBox);
    m_profileScheme->addItem(tr("Inherit"), QString());
    form->addRow(tr("Colour scheme"), m_profileScheme);

    m_profileFont = new QComboBox(m_profileFormBox);
    m_profileFont->addItem(tr("Inherit"), QString());
    for (const QString &f : monoFamilies())
        m_profileFont->addItem(f, f);
    form->addRow(tr("Font"), m_profileFont);

    m_profileFontSize = new QDoubleSpinBox(m_profileFormBox);
    m_profileFontSize->setRange(kFontSliderMin, kFontSliderMax);
    m_profileFontSize->setSingleStep(0.5);
    m_profileFontSize->setDecimals(1);
    // A spin box cannot express "no value", so the minimum doubles as the
    // inherit sentinel and the label spells it out.
    m_profileFontSize->setSpecialValueText(tr("Inherit"));
    m_profileFontSize->setValue(kFontSliderMin);
    form->addRow(tr("Font size"), m_profileFontSize);

    m_profileShell = new QLineEdit(m_profileFormBox);
    m_profileShell->setPlaceholderText(tr("Inherit (login shell)"));
    m_profileShell->setToolTip(tr("Full path to a shell or program, e.g. /usr/bin/fish"));
    form->addRow(tr("Shell program"), m_profileShell);

    m_profileShellArgs = new QLineEdit(m_profileFormBox);
    m_profileShellArgs->setPlaceholderText(tr("Inherit (no arguments)"));
    m_profileShellArgs->setToolTip(tr("Space-separated arguments for the shell program"));
    form->addRow(tr("Shell arguments"), m_profileShellArgs);

    m_profileWorkDir = new QLineEdit(m_profileFormBox);
    m_profileWorkDir->setPlaceholderText(tr("Inherit (home directory)"));
    form->addRow(tr("Start directory"), m_profileWorkDir);

    outer->addWidget(m_profileFormBox, 1);

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

    connect(m_profileAdd, &QPushButton::clicked, this, [this]() {
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
    });

    connect(m_profileDuplicate, &QPushButton::clicked, this, [this]() {
        const QString id = currentProfileId();
        if (id.isEmpty())
            return;
        const QString copy = m_profiles.duplicate(id);
        if (!copy.isEmpty())
            rebuildProfileList(copy);
    });

    connect(m_profileDelete, &QPushButton::clicked, this, [this]() {
        const QString id = currentProfileId();
        if (id.isEmpty())
            return;
        if (id == m_profiles.defaultProfile().id) {
            // The store refuses this too; say why rather than doing nothing.
            QMessageBox::information(
                this, tr("Cannot delete the default profile"),
                tr("The default profile is what “New Tab” opens, so it cannot be "
                   "deleted. You can rename it or change its settings."));
            return;
        }
        const auto answer = QMessageBox::question(
            this, tr("Delete profile?"),
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
    for (const auto &p : m_profiles.profiles()) {
        auto *item = new QListWidgetItem(
            p.isDefault ? tr("%1  (default)").arg(p.name) : p.name);
        item->setData(Qt::UserRole, p.id);
        if (p.isDefault) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
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

    // Inherit is index 0; a value no longer in the list (e.g. a scheme that has
    // been removed) falls back to Inherit rather than being silently rewritten.
    m_profileScheme->setCurrentIndex(
        p.colorScheme ? qMax(0, m_profileScheme->findData(*p.colorScheme)) : 0);
    m_profileFont->setCurrentIndex(
        p.fontFamily ? qMax(0, m_profileFont->findData(*p.fontFamily)) : 0);

    m_profileFontSize->setValue(p.fontSize ? *p.fontSize : kFontSliderMin);

    m_profileShell->setText(p.shellProgram ? *p.shellProgram : QString());
    m_profileShellArgs->setText(
        (p.shellProgram && p.shellArgs) ? p.shellArgs->join(QLatin1Char(' '))
                                        : QString());
    m_profileWorkDir->setText(p.workingDirectory ? *p.workingDirectory
                                                 : QString());

    // The arguments field only means something alongside a program.
    m_profileShellArgs->setEnabled(p.shellProgram.has_value());

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

    if (m_profileFontSize->value() <= kFontSliderMin)
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
    m_profileShellArgs->setEnabled(!shell.isEmpty());

    const QString dir = m_profileWorkDir->text().trimmed();
    p.workingDirectory = dir.isEmpty() ? std::nullopt
                                       : std::optional<QString>(dir);

    m_profiles.update(p);
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

    // Guard: these setters fire signals that write back to Settings. Setting
    // the widget value to what Settings already holds is a no-op, so the
    // round trip is harmless.
    m_schemeBox->setCurrentText(a.colorScheme);
    const int layoutIndex = m_tabLayoutBox->findData(a.tabLayout);
    if (layoutIndex >= 0)
        m_tabLayoutBox->setCurrentIndex(layoutIndex);

    if (const int fi = m_fontBox->findText(a.fontFamily); fi >= 0)
        m_fontBox->setCurrentIndex(fi);

    const int half = qRound(a.fontSize * 2);
    m_fontSizeSlider->setValue(half);
    m_fontSizeLabel->setText(formatPoints(a.fontSize));

    const int pct = qRound(a.opacity * 100);
    m_opacitySlider->setValue(pct);
    m_opacityLabel->setText(tr("%1%").arg(pct));

    m_boldBox->setChecked(a.boldIntense);
    m_italicBox->setChecked(a.italic);
    m_antialiasBox->setChecked(a.antialias);
    m_blinkBox->setChecked(a.blinkingCursor);
    m_lineCharsBox->setChecked(a.drawLineChars);
    m_scrollbackBox->setValue(s.scrollback);

    const int cursorIndex = m_cursorShapeBox->findData(QString::number(a.cursorShape));
    if (cursorIndex >= 0)
        m_cursorShapeBox->setCurrentIndex(cursorIndex);
    m_marginBox->setValue(a.terminalMargin);
    m_bidiBox->setChecked(a.bidi);

    const int sbIndex = m_scrollbarPosBox->findData(s.scrollbarPosition);
    if (sbIndex >= 0)
        m_scrollbarPosBox->setCurrentIndex(sbIndex);
    m_flowControlWarnBox->setChecked(s.flowControlWarning);
    m_confirmCloseBox->setChecked(s.confirmCloseWithRunningProcesses);

    m_monitorActivityBox->setChecked(s.monitorActivity);
    m_monitorSilenceBox->setChecked(s.monitorSilence);
    m_silenceTimeoutBox->setValue(s.silenceTimeout);
    m_silenceTimeoutBox->setEnabled(s.monitorSilence);
}
