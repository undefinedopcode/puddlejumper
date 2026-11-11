/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settingsdialog.h"

#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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

SettingsDialog::SettingsDialog(Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(tr("Settings"));
    setModal(true);
    resize(680, 470);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_sections = new QListWidget(this);
    m_sections->setFixedWidth(170);
    m_sections->addItems({tr("Appearance"), tr("Text"), tr("Cursor"), tr("Scrolling")});
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
    m_schemeBox->addItems(QTermWidget::availableColorSchemes());
    connect(m_schemeBox, &QComboBox::currentTextChanged, this, [this](const QString &v) {
        m_settings.setColorScheme(v);
    });
    form->addRow(tr("Colour scheme"), m_schemeBox);

    m_tabLayoutBox = new QComboBox(page);
    m_tabLayoutBox->addItem(tr("Tabs along the top"), QStringLiteral("standard"));
    m_tabLayoutBox->addItem(tr("Thumbnail sidebar"), QStringLiteral("thumbnails"));
    connect(m_tabLayoutBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_settings.setTabLayout(m_tabLayoutBox->itemData(i).toString());
    });
    form->addRow(tr("Tab layout"), m_tabLayoutBox);

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

    auto *lineChars = new QCheckBox(tr("Terminal draws its own box characters"), page);
    lineChars->setChecked(true);
    lineChars->setEnabled(false); // always on in qtermwidget
    form->addRow(QString(), lineChars);

    auto *lineHint = new QLabel(tr("Always on. Box frames and block art are painted "
                                   "as vectors rather than taken from the font, so "
                                   "they stay crisp at any point size."), page);
    lineHint->setWordWrap(true);
    lineHint->setEnabled(false);
    form->addRow(QString(), lineHint);

    return page;
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

    m_boldBox->setChecked(a.boldIntense);
    m_italicBox->setChecked(a.italic);
    m_antialiasBox->setChecked(a.antialias);
    m_blinkBox->setChecked(a.blinkingCursor);
    m_scrollbackBox->setValue(s.scrollback);
}
