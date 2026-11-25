/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexsession: RexSession, the class behind Rex tabs, against a running
 * Rex-compatible server, with real TerminalTab/TerminalWidget widgets on the
 * offscreen platform. Throughout, every pane is checked against the server:
 * its block's size is the pane's, and its text, history included, is the
 * server's.
 *
 *   a new session: a tab, sized from the widget, typed into;
 *   a new window: a second tab; splits, both ways: panes, each sized;
 *   another client splitting, resizing a split and renaming: followed;
 *   closing a pane and a window; a shell exiting in a split, and failing at
 *   once (kept to be read, then closed);
 *   detaching and attaching again: the windows, splits and history back;
 *   ending the session; nothing left behind on the server.
 *
 * Use a server of its own on loopback (it creates and destroys sessions):
 *   tuxosaurus --listen 127.0.0.1:51499 --state-dir /tmp/x --shell /bin/sh
 *   ./build/test-rexsession http://127.0.0.1:51499
 */

#include "qtermprobe.h"
#include "rexclient.h"
#include "rexsession.h"
#include "settings.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#include "ScreenWindow.h"
#include "TerminalDisplay.h"

namespace {

int failures = 0;
int checks = 0;

bool check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL: " << what << "\n";
        ++failures;
    }
    return ok;
}

bool waitFor(const std::function<bool()> &done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

Konsole::Screen *screenOf(TerminalWidget *pane)
{
    auto *d = pane ? pane->findChild<Konsole::TerminalDisplay *>() : nullptr;
    return d && d->screenWindow() ? d->screenWindow()->screen() : nullptr;
}

QSize screenSize(TerminalWidget *pane)
{
    Konsole::Screen *s = screenOf(pane);
    return s ? QSize(s->getColumns(), s->getLines()) : QSize();
}

QStringList paneLines(TerminalWidget *pane)
{
    Konsole::Screen *s = screenOf(pane);
    if (!s)
        return {};
    QStringList l = QtermProbeLines::screenLines(s);
    while (!l.isEmpty() && l.last().isEmpty())
        l.removeLast();
    return l;
}

bool shows(TerminalWidget *pane, const QString &line)
{
    return paneLines(pane).contains(line);
}

// A control connection of the test's own: what the server has, and
// changes made as another client would.
struct Server
{
    RexControl control;
    QSet<QString> attached;

    QJsonObject call(const QString &method, const QJsonObject &payload, bool quiet = false)
    {
        QJsonObject result, err;
        bool done = false;
        control.request(method, payload, [&](const QJsonObject &p, const QJsonObject &e) {
            result = p;
            err = e;
            done = true;
        });
        waitFor([&] { return done; });
        if (!err.isEmpty() && !quiet)
            check(false, method + QStringLiteral(": ") + QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact)));
        return result;
    }

    // Block methods need the connection attached to the session.
    void attach(const QString &sid)
    {
        if (!attached.contains(sid))
            call(QStringLiteral("session.attach"), QJsonObject{{QStringLiteral("session_id"), sid}});
        attached.insert(sid);
    }

    QJsonObject blockCall(const QString &sid, const QString &bid, const QString &method, const QJsonObject &args = {})
    {
        attach(sid);
        return call(method, QJsonObject{{QStringLiteral("session_id"), sid},
                                        {QStringLiteral("block_id"), bid},
                                        {QStringLiteral("args"), args}});
    }

    QStringList lines(const QString &sid, const QString &bid)
    {
        QStringList l = blockCall(sid, bid, QStringLiteral("com.superlogical.terminal.format"),
                                  QJsonObject{{QStringLiteral("trim"), true}})
                            .value(QLatin1String("content")).toString().split(QLatin1Char('\n'));
        while (!l.isEmpty() && l.last().isEmpty())
            l.removeLast();
        return l;
    }

    QSize size(const QString &sid, const QString &bid)
    {
        const QJsonObject r = blockCall(sid, bid, QStringLiteral("com.superlogical.terminal.size"));
        return QSize(r.value(QLatin1String("columns")).toInt(), r.value(QLatin1String("rows")).toInt());
    }

    QJsonObject view(const QString &sid)
    {
        attach(sid);
        return call(QStringLiteral("session.view"), QJsonObject{{QStringLiteral("session_id"), sid}});
    }

    QStringList sessionIds()
    {
        QStringList ids;
        for (const QJsonValue &v : call(QStringLiteral("session.list"), {}).value(QLatin1String("sessions")).toArray())
            ids << v.toObject().value(QLatin1String("session_id")).toString();
        return ids;
    }
};

// The client: a RexSession and the tabs it hands over, shown at a size.
struct Client
{
    RexSession *session = nullptr;
    QList<QPointer<TerminalTab>> tabs;
    QHash<TerminalTab *, QString> names;
    QString ended;
    bool hasEnded = false;

    void open(const QUrl &url, const QString &sid, const Settings &settings, const TerminalProfile &profile)
    {
        session = new RexSession(url, sid, settings, profile);
        QObject::connect(session, &RexSession::tabAdded, [this](TerminalTab *tab, const QString &name) {
            tabs << tab;
            names.insert(tab, name);
            tab->resize(900, 520);
            tab->show();
        });
        QObject::connect(session, &RexSession::tabRenamed, [this](TerminalTab *tab, const QString &name) {
            names.insert(tab, name);
        });
        QObject::connect(session, &RexSession::tabRemoved, [this](TerminalTab *tab) {
            tabs.removeAll(tab);
            names.remove(tab);
            tab->hide();
            tab->deleteLater();
        });
        QObject::connect(session, &RexSession::ended, [this](const QString &reason) {
            ended = reason;
            hasEnded = true;
        });
        session->start();
    }

    void close()
    {
        delete session;
        session = nullptr;
        tabs.clear();
        names.clear();
        hasEnded = false;
        ended.clear();
    }

    QList<TerminalWidget *> panes() const
    {
        QList<TerminalWidget *> out;
        for (const auto &t : tabs)
            if (t)
                out << t->panes();
        return out;
    }
};

// Each pane's block the size of the pane's screen, and the pane showing
// what the server has.
bool agrees(Server &server, Client &c, const QString &when)
{
    bool ok = true;
    const QString sid = c.session->sessionId();
    for (TerminalWidget *pane : c.panes()) {
        const QString bid = c.session->blockId(pane);
        QStringList want, got;
        QSize serverSize, paneSize;
        const bool same = waitFor([&] {
            serverSize = server.size(sid, bid);
            paneSize = screenSize(pane);
            want = server.lines(sid, bid);
            got = paneLines(pane);
            return serverSize == paneSize && want == got;
        }, 6000);
        if (!check(same, QStringLiteral("%1: pane %2 agrees with its block (pane %3x%4, block %5x%6)")
                             .arg(when, bid.right(8)).arg(paneSize.width()).arg(paneSize.height())
                             .arg(serverSize.width()).arg(serverSize.height()))) {
            ok = false;
            QTextStream err(stderr);
            err << "    pane:\n";
            for (const QString &l : got.mid(qMax(0, got.size() - 8)))
                err << "      |" << l << "|\n";
            err << "    server:\n";
            for (const QString &l : want.mid(qMax(0, want.size() - 8)))
                err << "      |" << l << "|\n";
        }
    }
    return ok;
}

void type(TerminalWidget *pane, const QString &text)
{
    pane->sendText(text);
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);  // leave the real settings alone
    QApplication app(argc, argv);
    const QString address = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("PJ_REX_SERVER");
    if (address.isEmpty()) {
        QTextStream(stdout) << "usage: test-rexsession http://host:port (or PJ_REX_SERVER); skipped\n";
        return 0;
    }
    const QUrl url(address);
    Settings settings;
    const TerminalProfile profile;

    Server server;
    server.control.open(url);
    if (!check(waitFor([&] { return server.control.isReady(); }), QStringLiteral("test's control connection")))
        return 1;
    const QStringList before = server.sessionIds();

    // ---- A new session: one tab, sized from the widget, typed into.
    Client c;
    c.open(url, QString(), settings, profile);
    if (!check(waitFor([&] { return c.tabs.size() == 1 || c.hasEnded; }) && c.tabs.size() == 1,
               QStringLiteral("a tab: ") + c.ended))
        return 1;
    const QString sid = c.session->sessionId();
    TerminalWidget *first = c.tabs.first()->activePane();
    waitFor([&] { return paneLines(first).join(QLatin1Char('\n')).contains(QLatin1Char('$')); }, 3000);
    type(first, QStringLiteral("i=1; while [ $i -le 40 ]; do echo \"one $i\"; i=$((i+1)); done; echo DONE_ONE\r"));
    check(waitFor([&] { return shows(first, QStringLiteral("DONE_ONE")); }), QStringLiteral("typed and echoed"));
    agrees(server, c, QStringLiteral("first window"));

    // ---- A new window: a second tab.
    c.session->newWindow();
    check(waitFor([&] { return c.tabs.size() == 2; }), QStringLiteral("a second tab"));
    TerminalTab *second = c.tabs.value(1);
    check(second && c.names.value(second) == QLatin1String("Shell"), QStringLiteral("named as the server says"));

    // ---- Splits, both ways.
    if (second) {
        c.session->splitPane(second->activePane(), Qt::Horizontal);
        check(waitFor([&] { return second->panes().size() == 2; }), QStringLiteral("split across: two panes"));
        c.session->splitPane(second->panes().at(1), Qt::Vertical);
        check(waitFor([&] { return second->panes().size() == 3; }), QStringLiteral("split down: three panes"));
        const QList<TerminalWidget *> panes = second->panes();
        for (int i = 0; i < panes.size(); ++i)
            type(panes.at(i), QStringLiteral("echo PANE_%1\r").arg(i));
        for (int i = 0; i < panes.size(); ++i)
            check(waitFor([&] { return shows(panes.at(i), QStringLiteral("PANE_%1").arg(i)); }),
                  QStringLiteral("pane %1 typed into").arg(i));
        const QSize a = screenSize(panes.at(0)), b = screenSize(panes.at(1)), d = screenSize(panes.at(2));
        check(a.height() == b.height() + d.height() + 1 && qAbs(a.width() - b.width()) <= 1,
              QStringLiteral("cells add up: %1x%2 | %3x%4 / %5x%6")
                  .arg(a.width()).arg(a.height()).arg(b.width()).arg(b.height()).arg(d.width()).arg(d.height()));
        agrees(server, c, QStringLiteral("split"));
    }

    // ---- Another client changes the layout: followed.
    if (second) {
        const QJsonArray windows = server.view(sid).value(QLatin1String("windows")).toArray();
        const QString windowId = windows.last().toObject().value(QLatin1String("window_id")).toString();
        server.call(QStringLiteral("session.set_window_label"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("window_id"), windowId},
                                {QStringLiteral("label"), QStringLiteral("renamed elsewhere")}});
        check(waitFor([&] { return c.names.value(second) == QLatin1String("renamed elsewhere"); }),
              QStringLiteral("a rename elsewhere is followed"));

        const QString left = c.session->blockId(second->panes().at(0));
        const QString right = c.session->blockId(second->panes().at(1));
        const int widthBefore = screenSize(second->panes().at(0)).width();
        server.call(QStringLiteral("session.resize_pane"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("before_block_id"), left},
                                {QStringLiteral("after_block_id"), right},
                                {QStringLiteral("direction"), QStringLiteral("horizontal")},
                                {QStringLiteral("ratio"), 0.3}});
        check(waitFor([&] { return screenSize(second->panes().at(0)).width() < widthBefore - 10; }),
              QStringLiteral("a split resized elsewhere is followed"));
        agrees(server, c, QStringLiteral("resized elsewhere"));

        server.call(QStringLiteral("session.new_split"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("window_id"), windowId},
                                {QStringLiteral("anchor_block_id"), left},
                                {QStringLiteral("direction"), QStringLiteral("vertical")},
                                {QStringLiteral("layout"), QJsonObject{{QStringLiteral("block"),
                                    QJsonObject{{QStringLiteral("flavor"), QStringLiteral("com.superlogical.terminal.shell")}}}}}});
        check(waitFor([&] { return second->panes().size() == 4; }), QStringLiteral("a split elsewhere is followed"));
        agrees(server, c, QStringLiteral("split elsewhere"));
    }

    // ---- Closing a pane, and a shell exiting in a split.
    if (second) {
        TerminalWidget *victim = second->panes().last();
        const QString victimBlock = c.session->blockId(victim);
        c.session->closePane(victim);
        check(waitFor([&] { return second->panes().size() == 3; }), QStringLiteral("a pane closed"));
        check(!QJsonDocument(server.view(sid)).toJson().contains(victimBlock.toUtf8()),
              QStringLiteral("its block gone from the server"));

        QThread::msleep(300);  // past the server's quick-exit threshold
        type(second->panes().last(), QStringLiteral("exit 3\r"));
        check(waitFor([&] { return second->panes().size() == 2; }), QStringLiteral("an exited shell's pane goes"));
        agrees(server, c, QStringLiteral("after an exit"));
    }

    // ---- Failing at once: kept to be read, then closed.
    if (second) {
        const QList<TerminalWidget *> old = second->panes();
        c.session->splitPane(old.first(), Qt::Vertical);
        check(waitFor([&] { return second->panes().size() == 3; }), QStringLiteral("another split"));
        TerminalWidget *quick = nullptr;
        for (TerminalWidget *p : second->panes())
            if (!old.contains(p))
                quick = p;
        if (check(quick != nullptr, QStringLiteral("the new pane"))) {
            type(quick, QStringLiteral("echo QUICK_FAIL; exit 5\r"));
            check(waitFor([&] { return c.session->hasExited(quick); }), QStringLiteral("exited"));
            check(waitFor([&] { return shows(quick, QStringLiteral("QUICK_FAIL")); }), QStringLiteral("its output shown"));
            waitFor([&] { return second->panes().size() != 3; }, 1000);
            check(second->panes().size() == 3, QStringLiteral("kept to be read"));
            c.session->closePane(quick);
            check(waitFor([&] { return second->panes().size() == 2; }), QStringLiteral("then closed"));
        }
    }

    // ---- Zoom: one pane fills the window, the others kept hidden (still
    // fed), then back; and zoomed from another client.
    if (second && second->panes().size() == 2) {
        const QList<TerminalWidget *> two = second->panes();
        const QSize unzoomed = screenSize(two.last());
        c.session->toggleZoom(two.last());
        check(waitFor([&] { return c.session->isZoomed(second) && second->panes().size() == 1; }),
              QStringLiteral("zoomed: one pane shown (zoomed %1, %2 panes, server %3)")
                  .arg(c.session->isZoomed(second)).arg(second->panes().size())
                  .arg(QString::fromUtf8(QJsonDocument(server.view(sid)).toJson(QJsonDocument::Compact)).contains(QLatin1String("zoomed_block_id"))));
        const QSize zoomed = screenSize(two.last());
        check(second->panes().value(0) == two.last()
                  && zoomed.width() * zoomed.height() > unzoomed.width() * unzoomed.height()
                  && zoomed.width() >= unzoomed.width() && zoomed.height() >= unzoomed.height(),
              QStringLiteral("the zoomed pane fills the window (%1x%2, was %3x%4)")
                  .arg(zoomed.width()).arg(zoomed.height()).arg(unzoomed.width()).arg(unzoomed.height()));
        agrees(server, c, QStringLiteral("zoomed"));
        type(two.last(), QStringLiteral("echo ZOOMED_IN\r"));
        check(waitFor([&] { return shows(two.last(), QStringLiteral("ZOOMED_IN")); }), QStringLiteral("typed into, zoomed"));
        // The hidden pane is kept, and still gets its output.
        const QString hiddenBlock = c.session->blockId(two.first());
        check(c.session->paneForBlock(hiddenBlock) == two.first(), QStringLiteral("the hidden pane kept"));
        server.blockCall(sid, hiddenBlock, QStringLiteral("com.superlogical.terminal.write"),
                         QJsonObject{{QStringLiteral("data"), QStringLiteral("echo WHILE_HIDDEN\r")}});
        check(waitFor([&] { return shows(two.first(), QStringLiteral("WHILE_HIDDEN")); }),
              QStringLiteral("a hidden pane still fed"));
        c.session->toggleZoom(two.last());
        check(waitFor([&] { return !c.session->isZoomed(second) && second->panes() == two; }),
              QStringLiteral("unzoomed: both back, the same panes"));
        agrees(server, c, QStringLiteral("unzoomed"));

        server.call(QStringLiteral("session.zoom_block"),
                    QJsonObject{{QStringLiteral("session_id"), sid},
                                {QStringLiteral("block_id"), hiddenBlock}, {QStringLiteral("zoom"), true}});
        check(waitFor([&] { return second->panes() == QList<TerminalWidget *>{two.first()}; }),
              QStringLiteral("zoomed elsewhere is followed"));
        server.call(QStringLiteral("session.zoom_block"),
                    QJsonObject{{QStringLiteral("session_id"), sid},
                                {QStringLiteral("block_id"), hiddenBlock}, {QStringLiteral("zoom"), false}});
        check(waitFor([&] { return second->panes() == two; }), QStringLiteral("and unzoomed elsewhere"));
        agrees(server, c, QStringLiteral("unzoomed elsewhere"));
    }

    // ---- Floating layers: floated out, typed into, moved and resized here
    // and elsewhere, another made elsewhere, raised, closed.
    const auto layersOf = [&](const QString &windowLabel) {
        QList<QPair<QString, QRectF>> out;
        for (const QJsonValue &wv : server.view(sid).value(QLatin1String("windows")).toArray()) {
            const QJsonObject wo = wv.toObject();
            if (wo.value(QLatin1String("label")).toString() != windowLabel)
                continue;
            for (const QJsonValue &lv : wo.value(QLatin1String("layers")).toArray()) {
                const QJsonObject lo = lv.toObject();
                if (lo.value(QLatin1String("kind")).toString() != QLatin1String("floating"))
                    continue;
                const QJsonObject b = lo.value(QLatin1String("bounds")).toObject();
                out.append({lo.value(QLatin1String("layer_id")).toString(),
                            QRectF(b.value(QLatin1String("x")).toDouble(), b.value(QLatin1String("y")).toDouble(),
                                   b.value(QLatin1String("w")).toDouble(), b.value(QLatin1String("h")).toDouble())});
            }
        }
        return out;
    };
    if (second) {
        const QString label = QStringLiteral("renamed elsewhere");
        TerminalWidget *floater = second->panes().last();
        c.session->floatPane(floater);
        check(waitFor([&] { return !second->floatingLayerOf(floater).isEmpty(); }), QStringLiteral("a pane floated"));
        check(second->panes().size() == 2 && c.session->isFloating(floater), QStringLiteral("still two panes, one floating"));
        const QString layer = second->floatingLayerOf(floater);
        check(layersOf(label).size() == 1 && layersOf(label).value(0).first == layer,
              QStringLiteral("the server has the layer"));
        const QSize floatedSize = screenSize(floater);
        const QSize tiledSize = screenSize(second->panes().first());
        check(tiledSize.width() > floatedSize.width(), QStringLiteral("the tiled pane has the window back (%1 > %2)")
                                                           .arg(tiledSize.width()).arg(floatedSize.width()));
        type(floater, QStringLiteral("echo FLOAT_OK\r"));
        check(waitFor([&] { return shows(floater, QStringLiteral("FLOAT_OK")); }), QStringLiteral("floating pane typed into"));
        agrees(server, c, QStringLiteral("floated"));

        // Resized elsewhere.
        server.call(QStringLiteral("session.resize_layer"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("layer_id"), layer},
                                {QStringLiteral("bounds"), QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0},
                                                                       {QStringLiteral("w"), 0.5}, {QStringLiteral("h"), 0.5}}}});
        check(waitFor([&] { return screenSize(floater).width() < floatedSize.width(); }),
              QStringLiteral("a layer resized elsewhere is followed"));
        agrees(server, c, QStringLiteral("layer resized elsewhere"));

        // Moved here (as dragging its title does): the server's bounds follow,
        // the size kept.
        QWidget *frame = floater;
        while (frame && frame->parentWidget() != second)
            frame = frame->parentWidget();
        const QSize frameSize = frame ? frame->size() : QSize();
        emit second->floatingGeometryChanged(layer, QRect(QPoint(second->layoutArea().width() / 4,
                                                                 second->layoutArea().height() / 4),
                                                          frameSize));
        check(waitFor([&] {
                  const QRectF b = layersOf(label).value(0).second;
                  return qAbs(b.x() - 0.25) < 0.02 && qAbs(b.y() - 0.25) < 0.02;
              }), QStringLiteral("moved here: the server's bounds follow"));
        const QRectF moved = layersOf(label).value(0).second;
        check(moved.width() == 0.5 && moved.height() == 0.5,
              QStringLiteral("moved, not resized: size kept (%1 x %2)").arg(moved.width()).arg(moved.height()));
        QPoint at;
        check(frame && waitFor([&] {
                  at = frame->geometry().topLeft() - second->layoutRect().topLeft();
                  const QPoint want(second->layoutArea().width() / 4, second->layoutArea().height() / 4);
                  return (at - want).manhattanLength() <= 2;
              }), QStringLiteral("the frame where the server put it (%1,%2)").arg(at.x()).arg(at.y()));

        // Resized here (as dragging its border does).
        emit second->floatingGeometryChanged(layer, QRect(frame->geometry().topLeft() - second->layoutRect().topLeft(),
                                                          frameSize + QSize(120, 60)));
        check(waitFor([&] { return layersOf(label).value(0).second.width() > 0.55; }),
              QStringLiteral("resized here: the server's bounds follow"));
        agrees(server, c, QStringLiteral("layer resized here"));
        check(shows(floater, QStringLiteral("FLOAT_OK")) || paneLines(floater).join(QString()).contains(QLatin1Char('$')),
              QStringLiteral("resized here: still shows its shell"));
        agrees(server, c, QStringLiteral("layer moved here"));

        // Another layer made elsewhere; then the first raised by using it.
        const QJsonArray windows = server.view(sid).value(QLatin1String("windows")).toArray();
        QString windowId;
        for (const QJsonValue &wv : windows)
            if (wv.toObject().value(QLatin1String("label")).toString() == label)
                windowId = wv.toObject().value(QLatin1String("window_id")).toString();
        server.call(QStringLiteral("session.new_layer"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("window_id"), windowId},
                                {QStringLiteral("bounds"), QJsonObject{{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5},
                                                                       {QStringLiteral("w"), 0.45}, {QStringLiteral("h"), 0.45}}},
                                {QStringLiteral("layout"), QJsonObject{{QStringLiteral("block"),
                                    QJsonObject{{QStringLiteral("flavor"), QStringLiteral("com.superlogical.terminal.shell")}}}}}});
        check(waitFor([&] { return second->panes().size() == 3; }), QStringLiteral("a layer made elsewhere is shown"));
        agrees(server, c, QStringLiteral("layer made elsewhere"));
        check(layersOf(label).size() == 2 && layersOf(label).last().first != layer, QStringLiteral("the new layer on top"));
        // PJ_REX_SHOT=file.png: the tab with its two layers, to look at.
        if (const QByteArray shot = qgetenv("PJ_REX_SHOT"); !shot.isEmpty()) {
            QThread::msleep(300);
            QCoreApplication::processEvents();
            second->grab().save(QString::fromLocal8Bit(shot));
        }
        second->setActivePane(floater);
        check(waitFor([&] { return layersOf(label).size() == 2 && layersOf(label).last().first == layer; }),
              QStringLiteral("using a pane raises its layer"));

        // The other closed from its frame.
        const QString other = layersOf(label).value(0).first;
        emit second->floatingCloseRequested(other);
        check(waitFor([&] { return second->panes().size() == 2 && layersOf(label).size() == 1; }),
              QStringLiteral("a layer closed"));
        agrees(server, c, QStringLiteral("layer closed"));
    }

    // ---- Detached and attached again: the same windows, splits, history.
    const int panesBefore = int(c.panes().size());
    c.session->detach();
    check(waitFor([&] { return c.hasEnded; }) && c.tabs.isEmpty(), QStringLiteral("detached: tabs removed"));
    c.close();
    check(server.sessionIds().contains(sid), QStringLiteral("the session is still there"));

    c.open(url, sid, settings, profile);
    check(waitFor([&] { return c.tabs.size() == 2 && int(c.panes().size()) == panesBefore; }),
          QStringLiteral("attached again: two tabs, %1 panes").arg(panesBefore));
    check(c.names.value(c.tabs.value(1)) == QLatin1String("renamed elsewhere"), QStringLiteral("names back"));
    check(waitFor([&] { return shows(c.panes().value(0), QStringLiteral("one 1")); }), QStringLiteral("history back"));
    agrees(server, c, QStringLiteral("attached again"));
    {
        TerminalTab *again = c.tabs.value(1);
        TerminalWidget *floater = nullptr;
        for (TerminalWidget *p : again ? again->panes() : QList<TerminalWidget *>())
            if (c.session->isFloating(p))
                floater = p;
        if (check(floater != nullptr, QStringLiteral("attached again: still floating"))) {
            c.session->dockPane(floater);
            check(waitFor([&] { return !c.session->isFloating(floater) && again->panes().size() == 2; }),
                  QStringLiteral("docked back into the tiled layout"));
            check(layersOf(QStringLiteral("renamed elsewhere")).isEmpty(), QStringLiteral("no floating layer left"));
            agrees(server, c, QStringLiteral("docked"));
        }
    }
    // A window whose only pane floats: still a tab, nothing tiled, and not
    // rebuilt on every change.
    if (TerminalTab *only = c.tabs.value(0); only && only->panes().size() == 1) {
        TerminalWidget *pane = only->panes().first();
        c.session->floatPane(pane);
        check(waitFor([&] { return c.session->isFloating(pane); }), QStringLiteral("the only pane floated"));
        check(c.tabs.size() == 2 && only->panes() == QList<TerminalWidget *>{pane},
              QStringLiteral("its tab stays, the same pane"));
        agrees(server, c, QStringLiteral("only pane floating"));
        QPointer<QWidget> holder = pane->parentWidget();
        c.session->renameWindow(only, QStringLiteral("all floating"));
        check(waitFor([&] { return c.names.value(only) == QLatin1String("all floating"); }),
              QStringLiteral("a change to the window"));
        check(holder && pane->parentWidget() == holder, QStringLiteral("not rebuilt for it"));
    }

    // ---- Closing a window.
    c.session->closeWindow(c.tabs.value(1));
    check(waitFor([&] { return c.tabs.size() == 1; }), QStringLiteral("a window closed"));

    // ---- Ending the session.
    c.session->destroySession();
    check(waitFor([&] { return c.hasEnded; }) && c.tabs.isEmpty(), QStringLiteral("ended: tabs removed"));
    c.close();
    check(waitFor([&] { return server.sessionIds() == before; }, 3000), QStringLiteral("nothing left behind"));

    // ---- The last shell exiting ends a session.
    c.open(url, QString(), settings, profile);
    if (check(waitFor([&] { return c.tabs.size() == 1; }), QStringLiteral("a tab for exit"))) {
        QThread::msleep(400);
        type(c.tabs.first()->activePane(), QStringLiteral("exit 4\r"));
        check(waitFor([&] { return c.hasEnded; }) && c.tabs.isEmpty(), QStringLiteral("the last shell exiting ends it"));
        QTextStream(stdout) << "ended: " << c.ended << "\n";
        c.close();
    }
    check(waitFor([&] { return server.sessionIds() == before; }, 5000), QStringLiteral("and its session goes"));
    for (const QString &id : server.sessionIds())
        if (!before.contains(id)) {
            server.attach(id);
            server.call(QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), id}}, true);
        }

    server.control.close();
    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
