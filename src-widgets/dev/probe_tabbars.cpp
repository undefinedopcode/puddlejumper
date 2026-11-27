/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * probe-tabbars: draws the top tab strip and the sidebar's header, with the
 * workspace button shown, as the platform's style draws them: each at rest,
 * with the mouse over its workspace and new-tab buttons, and pressed. For
 * checking how the style paints those buttons (macOS's bezels).
 *
 *   ./build/probe-tabbars OUTDIR [STYLE]
 */

#include "tabstrip.h"
#include "thumbnailsidebar.h"

#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QToolButton>

namespace {

QList<QToolButton *> buttons(QWidget *w)
{
    QList<QToolButton *> out;
    for (QToolButton *b : w->findChildren<QToolButton *>()) {
        if (b->isVisibleTo(w))
            out << b;
    }
    return out;
}

// The widget at rest, hovered and pressed, stacked, on a grey so a bezel
// shows against it.
QImage states(QWidget *w)
{
    QList<QImage> shots;
    for (int state = 0; state < 3; ++state) {
        for (QToolButton *b : buttons(w)) {
            b->setAttribute(Qt::WA_UnderMouse, state >= 1);
            b->setDown(state == 2);
        }
        shots << w->grab().toImage();
    }
    QImage out(w->width() * shots.first().devicePixelRatio(), 3 * shots.first().height() + 8, QImage::Format_ARGB32);
    out.fill(QColor(128, 128, 128));
    QPainter p(&out);
    int y = 0;
    for (const QImage &s : shots) {
        p.drawImage(0, y, s);
        y += s.height() + 4;
    }
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    if (argc > 2)
        app.setStyle(QStyleFactory::create(QString::fromLocal8Bit(argv[2])));
    QDir().mkpath(outDir);
    QTextStream out(stdout);
    out << "style " << app.style()->name() << ", platform " << QGuiApplication::platformName() << "\n";

    QMenu menu;
    menu.addAction(QStringLiteral("Default"));
    const QIcon icon = app.style()->standardIcon(QStyle::SP_DirIcon);
    const QColor bg(0x1e, 0x1e, 0x2e), fg(0xcd, 0xd6, 0xf4);

    TabStrip strip;
    strip.setColours(bg, fg);
    strip.syncTabs({QStringLiteral("zsh"), QStringLiteral("build")}, 0);
    strip.setWorkspace(QStringLiteral("Default"), icon, &menu, true);
    strip.resize(520, strip.sizeHint().height());
    strip.show();

    ThumbnailSidebar sidebar;
    sidebar.setColours(bg, fg);
    sidebar.setWorkspace(QStringLiteral("Default"), icon, &menu, true);
    sidebar.resize(240, 120);
    sidebar.show();
    QCoreApplication::processEvents();

    for (auto [name, w] : {std::pair<const char *, QWidget *>{"tabstrip", &strip}, {"sidebar", &sidebar}}) {
        const QString path = outDir + QStringLiteral("/%1.png").arg(QLatin1String(name));
        states(w).save(path);
        out << name << ": " << buttons(w).size() << " buttons -> " << path << "\n";
    }
    return 0;
}
