/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "aboutdialog.h"

#include "cardstyle.h"
#include "popupframe.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSvgRenderer>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#ifndef PUDDLEJUMPER_GIT_COMMIT
#define PUDDLEJUMPER_GIT_COMMIT ""
#endif

namespace {

constexpr int kLogo = 72;
const char *const kHome = "https://github.com/undefinedopcode/puddlejumper";

// The logo (icons/portal-prompt.svg, drawn in currentColor) in `colour`.
QPixmap logo(const QColor &colour, qreal dpr)
{
    QFile f(QStringLiteral(":/icons/portal-prompt.svg"));
    QByteArray svg = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    svg.replace("currentColor", colour.name(QColor::HexRgb).toLatin1());
    QPixmap pm(QSize(kLogo, kLogo) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QSvgRenderer(svg).render(&p, QRectF(0, 0, kLogo, kLogo));
    return pm;
}

QString commit()
{
    return QString::fromLatin1(PUDDLEJUMPER_GIT_COMMIT);
}

QString versionLine()
{
    const QString c = commit();
    return c.isEmpty() ? QApplication::translate("AboutDialog", "Version %1").arg(QApplication::applicationVersion())
                       : QApplication::translate("AboutDialog", "Version %1 (%2)").arg(QApplication::applicationVersion(), c);
}

QString systemLine()
{
    return QApplication::translate("AboutDialog", "Qt %1 · %2 · %3")
        .arg(QString::fromLatin1(qVersion()), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
}

} // namespace

QString AboutDialog::versionInfo()
{
    const QString c = commit();
    return QStringLiteral("PuddleJumper %1%2\n%3\n")
        .arg(QApplication::applicationVersion(), c.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(c), systemLine());
}

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setWindowTitle(tr("About PuddleJumper"));

    const int s = popupframe::kShadow;
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(s + 32, s + 28, s + 32, s + 20);
    outer->setSpacing(0);

    QPalette quiet = palette();
    quiet.setColor(QPalette::WindowText, popupframe::muted(palette()));

    auto *mark = new QLabel(this);
    mark->setPixmap(logo(cardstyle::accent(), devicePixelRatioF()));
    mark->setAlignment(Qt::AlignHCenter);
    outer->addWidget(mark);
    outer->addSpacing(14);

    auto *name = new QLabel(QStringLiteral("PuddleJumper"), this);
    QFont nameFont = name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.6);
    nameFont.setWeight(QFont::DemiBold);
    name->setFont(nameFont);
    name->setAlignment(Qt::AlignHCenter);
    outer->addWidget(name);
    outer->addSpacing(2);

    auto *version = new QLabel(versionLine(), this);
    version->setAlignment(Qt::AlignHCenter);
    version->setPalette(quiet);
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(version);
    outer->addSpacing(14);

    auto *what = new QLabel(tr("A terminal for local shells, tmux and Rex sessions, with VNC, RDP and web tabs "
                               "alongside."),
                            this);
    what->setWordWrap(true);
    what->setAlignment(Qt::AlignHCenter);
    outer->addWidget(what);
    outer->addSpacing(14);

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.88);
    const auto quietLine = [&](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setFont(small);
        l->setPalette(quiet);
        l->setAlignment(Qt::AlignHCenter);
        l->setWordWrap(true);
        outer->addWidget(l);
        return l;
    };
    quietLine(systemLine())->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addSpacing(2);
    quietLine(tr("GPL-2.0-or-later · built on QTermWidget · icons from Lucide"));
    outer->addSpacing(2);
    QLabel *link = quietLine(QStringLiteral("<a href=\"%1\">github.com/undefinedopcode/puddlejumper</a>")
                                 .arg(QString::fromLatin1(kHome)));
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);
    outer->addSpacing(20);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto *copy = new QPushButton(tr("Copy Version Info"), this);
    copy->setAutoDefault(false);
    auto *close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(copy, &QPushButton::clicked, this, [copy]() {
        QGuiApplication::clipboard()->setText(versionInfo());
        copy->setText(tr("Copied"));
        QTimer::singleShot(1500, copy, [copy]() { copy->setText(tr("Copy Version Info")); });
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addStretch();
    buttons->addWidget(copy);
    buttons->addWidget(close);
    buttons->addStretch();
    outer->addLayout(buttons);
    close->setFocus();
}

void AboutDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    popupframe::paint(p, rect(), palette());
}

void AboutDialog::showEvent(QShowEvent *event)
{
    // Centred over the window, a little above the middle.
    const int width = 420 + 2 * popupframe::kShadow;
    resize(width, heightForWidth(width) > 0 ? heightForWidth(width) : sizeHint().height());
    QWidget *over = parentWidget() ? parentWidget()->window() : nullptr;
    const QRect area = over ? over->geometry()
                            : (screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800));
    move(area.center().x() - width / 2, area.center().y() - height() / 2 - area.height() / 12);
    QDialog::showEvent(event);
}
