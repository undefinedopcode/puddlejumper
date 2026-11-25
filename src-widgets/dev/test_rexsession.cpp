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
