/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "workspace.h"

#include "appicon.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>

namespace {

// The accents on offer: distinct from one another on light and dark
// backgrounds, and none so strong it shouts.
const QList<QColor> &accents()
{
    static const QList<QColor> colours = {
        QColor(0xe5, 0x53, 0x4b), QColor(0xd6, 0x83, 0x3a), QColor(0xc9, 0xa2, 0x27),
        QColor(0x57, 0xa6, 0x4a), QColor(0x3a, 0xa6, 0xa0), QColor(0x4a, 0x8f, 0xe0),
        QColor(0x9b, 0x6d, 0xdb), QColor(0xd6, 0x5d, 0x9a),
    };
    return colours;
}

QIcon dotIcon(const QColor &fill, const QColor &outline)
{
    QIcon icon;
    for (const int size : {16, 24, 32}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal inset = size * 0.22;
        const QRectF r = QRectF(pm.rect()).adjusted(inset, inset, -inset, -inset);
        if (fill.isValid()) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
        } else {
            QColor line = outline;
            line.setAlphaF(0.6);
            p.setPen(QPen(line, size / 12.0));
            p.setBrush(Qt::NoBrush);
        }
        p.drawEllipse(r);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace

QIcon Workspace::icon(const QColor &outline) const
{
    return dotIcon(colour, outline);
}

bool WorkspaceDialog::edit(QWidget *parent, const QString &title, Workspace &ws)
{
    WorkspaceDialog dialog(title, ws, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    ws.name = dialog.m_name->text().trimmed();
    const QString root = dialog.m_root->text().trimmed();
    ws.root = root.startsWith(QLatin1Char('~')) ? QDir::homePath() + root.mid(1) : root;
    const int chosen = dialog.m_colours->checkedId();
    ws.colour = chosen > 0 ? accents().value(chosen - 1) : QColor();
    return true;
}

WorkspaceDialog::WorkspaceDialog(const QString &title, const Workspace &ws, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    auto *form = new QFormLayout(this);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_name = new QLineEdit(ws.name, this);
    m_name->setPlaceholderText(tr("e.g. the project's name"));
    form->addRow(tr("Name"), m_name);

    auto *rootRow = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(rootRow);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    m_root = new QLineEdit(ws.root, rootRow);
    m_root->setPlaceholderText(tr("The profile's (usually home)"));
    m_root->setMinimumWidth(280);
    auto *browse = new QToolButton(rootRow);
    browse->setIcon(appIcon("folder-open"));
    browse->setToolTip(tr("Choose a folder"));
    connect(browse, &QToolButton::clicked, this, [this]() {
        const QString start = m_root->text().isEmpty() ? QDir::homePath() : m_root->text();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Workspace Folder"), start);
        if (!dir.isEmpty()) {
            m_root->setText(dir);
            // A folder's name is usually the project's: offer it.
            if (m_name->text().trimmed().isEmpty())
                m_name->setText(QDir(dir).dirName());
        }
    });
    rootLayout->addWidget(m_root, 1);
    rootLayout->addWidget(browse);
    form->addRow(tr("Folder"), rootRow);

    // A row of swatches, the first meaning "no colour".
    auto *swatches = new QWidget(this);
    auto *swatchLayout = new QHBoxLayout(swatches);
    swatchLayout->setContentsMargins(0, 0, 0, 0);
    swatchLayout->setSpacing(2);
    m_colours = new QButtonGroup(this);
    const QColor outline = palette().color(QPalette::WindowText);
    for (int i = 0; i <= accents().size(); ++i) {
        auto *b = new QToolButton(swatches);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        b->setIcon(dotIcon(i == 0 ? QColor() : accents().at(i - 1), outline));
        b->setToolTip(i == 0 ? tr("No colour") : QString());
        m_colours->addButton(b, i);
        swatchLayout->addWidget(b);
        if ((i == 0 && !ws.colour.isValid()) || (i > 0 && accents().at(i - 1) == ws.colour))
            b->setChecked(true);
    }
    if (!m_colours->checkedButton())
        m_colours->button(0)->setChecked(true);
    swatchLayout->addStretch(1);
    form->addRow(tr("Colour"), swatches);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);

    // A workspace needs a name.
    const auto update = [this, buttons]() {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_name->text().trimmed().isEmpty());
    };
    connect(m_name, &QLineEdit::textChanged, this, update);
    update();
    m_name->setFocus();
}
