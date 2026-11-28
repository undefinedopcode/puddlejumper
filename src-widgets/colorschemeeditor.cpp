/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "colorschemeeditor.h"
#include "uiprompt.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

/* A swatch that paints its colour and names itself. */
class Swatch : public QPushButton
{
public:
    explicit Swatch(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setMinimumHeight(28);
        setCursor(Qt::PointingHandCursor);
        setFlat(false);
    }

    void setColor(const QColor &c)
    {
        m_color = c;
        setText(c.name().toUpper());
        // Choose readable label text from the swatch's luminance, so a pale
        // colour does not get white-on-white.
        const int luma = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        setStyleSheet(QStringLiteral(
                          "QPushButton { background: %1; color: %2; border: 1px solid palette(mid); }")
                          .arg(c.name(), luma > 140 ? QStringLiteral("#000")
                                                    : QStringLiteral("#fff")));
    }
    QColor color() const { return m_color; }

private:
    QColor m_color = Qt::black;
};

} // namespace

SchemePreview::SchemePreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 132);
}

void SchemePreview::setScheme(const ColorSchemeFile &s)
{
    m_scheme = s;
    update();
}

void SchemePreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const auto &c = m_scheme.colors;
    const bool hasScheme = c.size() == ColorSchemeFile::RoleCount;
    const QColor bg = hasScheme ? c[ColorSchemeFile::Background] : Qt::black;
    const QColor fg = hasScheme ? c[ColorSchemeFile::Foreground] : Qt::white;

    p.fillRect(rect(), bg);

    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    mono.setPointSizeF(9.5);
    p.setFont(mono);
    const QFontMetrics fm(mono);
    const int lh = fm.height();
    int y = 8;

    auto line = [&](const QString &text, QColor col) {
        p.setPen(col);
        p.drawText(8, y + fm.ascent(), text);
        y += lh;
    };

    line(QStringLiteral("user@host:~$ ls --color"), fg);
    if (hasScheme) {
        // Show enough of the palette to judge it as a set, not just as a
        // text/background pair.
        auto entry = [&](QColor nameColor, const QString &perm, const QString &name) {
            p.setPen(nameColor);
            p.drawText(8, y + fm.ascent(), perm);
            p.setPen(fg);
            p.drawText(8 + fm.horizontalAdvance(perm + QStringLiteral("  ")),
                       y + fm.ascent(), name);
            y += lh;
        };
        entry(c[ColorSchemeFile::Color4], QStringLiteral("drwxr-xr-x"), QStringLiteral("src"));
        entry(c[ColorSchemeFile::Color2], QStringLiteral("-rw-r--r--"), QStringLiteral("README.md"));
        entry(c[ColorSchemeFile::Color1], QStringLiteral("-rw-r--r--"), QStringLiteral("broken.c"));

        // The ANSI ramp, normal then bright -- the quickest sanity check for a
        // scheme whose intense colours have been neglected.
        const int sw = 14;
        int x = 8;
        for (int i = 0; i < 8; ++i) {
            p.fillRect(x, y, sw, sw - 2, c[ColorSchemeFile::Color0 + i]);
            x += sw + 2;
        }
        x = 8;
        y += sw;
        for (int i = 0; i < 8; ++i) {
            p.fillRect(x, y, sw, sw - 2, c[ColorSchemeFile::Color0Intense + i]);
            x += sw + 2;
        }
        y += sw;

        // Bold text uses the Intense foreground; showing it keeps that role
        // from being a blind spot.
        QFont bold = mono;
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(c[ColorSchemeFile::ForegroundIntense]);
        const QString boldSample = QStringLiteral("bold / intense text  ");
        p.drawText(8, y + fm.ascent(), boldSample);
        p.setFont(mono);

        // A block cursor in the foreground colour, as a real terminal draws.
        const int cw = fm.horizontalAdvance(QLatin1Char('M'));
        p.fillRect(8 + fm.horizontalAdvance(boldSample), y - 1, cw, lh - 2, fg);
    } else {
        line(QStringLiteral("(no scheme)"), fg);
    }
}

ColorSchemeEditor::ColorSchemeEditor(const QString &seedName, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Colour Scheme Editor"));
    setModal(true);

    /*
     * Seed from the requested scheme.
     *
     * load() only overwrites the roles a file actually defines, so start from
     * the built-in table and layer the file on top: a partial or old scheme
     * then keeps sane values for anything it omits.
     */
    m_scheme = ColorSchemeFile::defaultScheme();
    const QString seedPath = ColorSchemeFile::findSchemeFile(seedName);
    if (!seedPath.isEmpty())
        m_scheme.load(seedPath);

    /*
     * Copy, or edit in place?
     *
     * "Edit a copy" exists to protect schemes the user does not own. Applied
     * blindly it is just friction: having built a scheme and then wanting its
     * background a shade lighter, you get "X copy" instead of editing X. So the
     * user's own schemes are edited in place, and only built-in ones are
     * copied.
     */
    m_editingOwnScheme = ColorSchemeFile::isUserScheme(
        seedPath.isEmpty() ? seedName : seedPath);

    QString candidate;
    if (m_editingOwnScheme) {
        // Reuse the seed's own name. load() already set it from the filename.
        candidate = m_scheme.name;
        m_originalName = candidate;
    } else {
        QString base = seedName.isEmpty() ? tr("Custom") : seedName;
        candidate = tr("%1 copy").arg(base);
        int n = 2;
        const QStringList existing = ColorSchemeFile::userSchemeNames();
        while (existing.contains(candidate, Qt::CaseInsensitive))
            candidate = tr("%1 copy %2").arg(base).arg(n++);
    }
    m_scheme.name = candidate;
    m_scheme.description = candidate;

    auto *outer = new QVBoxLayout(this);
    outer->setSpacing(10);

    // ---- name ------------------------------------------------------------
    auto *header = new QFormLayout;
    m_nameEdit = new QLineEdit(candidate, this);
    m_nameEdit->setPlaceholderText(tr("Scheme name"));
    header->addRow(tr("Name"), m_nameEdit);
    outer->addLayout(header);

    auto *copyHint = new QLabel(
        m_editingOwnScheme
            ? tr("This is one of your own schemes: saving updates it in place. "
                 "Rename it to save as a new scheme instead.")
            : tr("You are editing a copy of a built-in scheme. Saving creates a "
                 "new scheme; the original is left untouched."),
        this);
    copyHint->setWordWrap(true);
    copyHint->setEnabled(false);
    outer->addWidget(copyHint);

    // ---- preview ---------------------------------------------------------
    auto *previewBox = new QGroupBox(tr("Preview"), this);
    auto *previewLayout = new QVBoxLayout(previewBox);
    m_preview = new SchemePreview(previewBox);
    previewLayout->addWidget(m_preview);
    outer->addWidget(previewBox);

    // ---- roles -----------------------------------------------------------
    auto *colorsBox = new QGroupBox(tr("Colours"), this);
    auto *colorsGrid = new QGridLayout(colorsBox);
    colorsGrid->setSpacing(6);

    // Sized up front and indexed by role: indexing an empty QVector with
    // operator[] would be out of bounds.
    m_swatches.resize(ColorSchemeFile::RoleCount);

    auto addSection = [&](const QString &title, int firstRole, int count,
                          int columns, int startRow) {
        auto *label = new QLabel(title, colorsBox);
        QFont f = label->font();
        f.setBold(true);
        label->setFont(f);
        colorsGrid->addWidget(label, startRow, 0, 1, columns);
        int row = startRow + 1;
        for (int i = 0; i < count; ++i) {
            const int role = firstRole + i;
            auto *sw = new Swatch(colorsBox);
            sw->setToolTip(ColorSchemeFile::roleKey(role));
            sw->setColor(m_scheme.colors.at(role));
            connect(sw, &QPushButton::clicked, this, [this, role] {
                pickColor(role);
            });
            m_swatches[role] = sw;
            colorsGrid->addWidget(sw, row + i / columns, i % columns);
        }
        return row + (count + columns - 1) / columns;
    };

    // Text/background first (what a scheme is judged on), then the two ANSI
    // blocks. Colour8..15 are the brights, so they get their own heading:
    // forgetting them is the most common way a scheme looks broken.
    int row = addSection(tr("Text and background"),
                         ColorSchemeFile::Foreground, 2, 2, 0);
    row = addSection(tr("ANSI colours"), ColorSchemeFile::Color0, 8, 4, row + 1);
    row = addSection(tr("ANSI colours (bright)"),
                     ColorSchemeFile::Color0Intense, 8, 4, row + 1);
    outer->addWidget(colorsBox);

    // ---- buttons ---------------------------------------------------------
    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(tr("&Save Scheme"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ColorSchemeEditor::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *resetBtn = buttons->addButton(tr("&Reset Colours"), QDialogButtonBox::ResetRole);
    connect(resetBtn, &QPushButton::clicked, this, [this, seedPath] {
        ColorSchemeFile fresh = ColorSchemeFile::defaultScheme();
        if (!seedPath.isEmpty())
            fresh.load(seedPath);
        m_scheme.colors = fresh.colors;
        refreshSwatches();
        refreshPreview();
    });

    outer->addWidget(buttons);

    refreshSwatches();
    refreshPreview();

    // Keep the preview honest as the name changes; the colours are what matter
    // but the name is what the file is called.
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_scheme.name = t.trimmed();
        m_scheme.description = m_scheme.name;
    });

    adjustSize();
}

void ColorSchemeEditor::pickColor(int role)
{
    const QColor current = m_scheme.colors.at(role);
    const QColor chosen = QColorDialog::getColor(
        current, this,
        tr("Choose colour for %1").arg(ColorSchemeFile::roleLabel(role)));
    if (!chosen.isValid())
        return;

    m_scheme.colors[role] = chosen;
    refreshSwatches();
    refreshPreview();
}

void ColorSchemeEditor::refreshSwatches()
{
    for (int i = 0; i < m_swatches.size(); ++i) {
        if (m_swatches.at(i))
            static_cast<Swatch *>(m_swatches.at(i))
                ->setColor(m_scheme.colors.at(i));
    }
}

void ColorSchemeEditor::refreshPreview()
{
    if (m_preview)
        m_preview->setScheme(m_scheme);
}

void ColorSchemeEditor::save()
{
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        uiprompt::message(this, tr("Name required"),
                             tr("Give the scheme a name before saving."));
        return;
    }
    // A name is also a filename, so keep it to characters that travel well.
    static const QRegularExpression invalid(QStringLiteral(R"([/\\:*?"<>|])"));
    if (invalid.match(name).hasMatch()) {
        uiprompt::message(this, tr("Invalid name"),
                             tr("A scheme name cannot contain / \\ : * ? \" < > |"));
        return;
    }

    m_scheme.name = name;
    m_scheme.description = name;

    const QString path = ColorSchemeFile::userSchemePath(name);

    /*
     * Overwriting one of the user's own schemes is the normal case for an
     * in-place edit, so it is not worth interrupting for. Asking only makes
     * sense when the overwrite is a surprise: the user typed the name of an
     * existing scheme that this dialog was not already editing.
     */
    const bool wasAlreadyThisScheme = m_editingOwnScheme
                                      && name == m_originalName;
    if (QFile::exists(path) && !wasAlreadyThisScheme) {
        if (!uiprompt::confirm(this, tr("Replace existing scheme?"),
                               tr("A scheme called “%1” already exists. Replace it?").arg(name), tr("Replace"), true))
            return;
    }

    const bool existed = QFile::exists(path);

    if (!m_scheme.save(path)) {
        uiprompt::message(this, tr("Could not save"),
                              tr("Could not write %1.").arg(path));
        return;
    }

    m_savedPath = path;
    m_savedName = name;
    // Only an overwrite of a scheme that already existed needs the cache
    // workaround; a brand-new name loads normally.
    m_overwroteExisting = existed;
    accept();
}
