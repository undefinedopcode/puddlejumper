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
 *   keymaps and actions: the keymap (this client's defaults merged with the
 *   server's), server actions run, the client's actions performed for the
 *   server, prompts answered and cancelled, a reloaded configuration;
 *   ending the session; nothing left behind on the server.
 *
 * Use a server of its own on loopback (it creates and destroys sessions):
 *   tuxosaurus --listen 127.0.0.1:51499 --state-dir /tmp/x --shell /bin/sh
 *   ./build/test-rexsession http://127.0.0.1:51499
 * The actions tests want a configuration with test actions (and skip
 * them without), and reload it, so it should be a copy somewhere temporary:
 *   cp src-widgets/dev/rex-test-keys.lua /tmp/x/keys.lua
 *   atlantis --config /tmp/x/keys.lua ...
 * atlantis reloads only for its local socket, or SIGHUP: give its PID in
 * PUDDLEJUMPER_TEST_REX_PID to test that.
 */

#include "qtermprobe.h"
#include "rexclient.h"
#include "rexsession.h"
#include "settings.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QCursor>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#include <csignal>

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
        // As the app does: its actions and their default keys.
        session->setKeymapRequest([] {
            return QJsonDocument::fromJson(R"({
              "actions": [{"name": "client.tab.new", "kind": "client", "title": "New Tab"},
                          {"name": "pane.copy", "kind": "client", "title": "Copy"}],
              "defaults": [{"name": "puddlejumper", "bindings": [
                  {"keys": ["ctrl+shift+t"], "action": "client.tab.new"},
                  {"keys": ["ctrl+shift+c"], "action": "pane.copy"}]}]
            })").object();
        });
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
    // Every pane inside its tab and shown there (not lost out of its frame).
    for (const auto &t : c.tabs) {
        for (TerminalWidget *pane : t ? t->panes() : QList<TerminalWidget *>())
            ok &= check(t->isAncestorOf(pane) && pane->isVisibleTo(t) && pane->window() == t->window(),
                        QStringLiteral("%1: pane %2 shown in its tab").arg(when, c.session->blockId(pane).right(8)));
    }
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
        // A block another client owns keeps that client's size; the pane
        // shows it, and says so.
        if (same && pane->isFollowing() != c.session->isFollowing(pane))
            ok &= check(false, QStringLiteral("%1: pane %2's following badge").arg(when, bid.right(8)));
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

/*
 * Keymaps and actions. With dev/rex-test-keys.lua as the server's
 * configuration, all of it; without, the keymap only.
 */
void keymapTests(Server &server, Client &c, Settings &settings)
{
    RexSession *s = c.session;
    QObject guard;
    int keymaps = 0;
    QStringList finished, failed;
    QList<QJsonValue> results;
    QString performCall, performAction, promptCall;
    QJsonObject performArgs, prompt;
    QObject::connect(s, &RexSession::keymapChanged, &guard, [&] { ++keymaps; });
    QObject::connect(s, &RexSession::actionFinished, &guard, [&](const QString &a, const QJsonValue &r) {
        finished << a;
        results << r;
    });
    QObject::connect(s, &RexSession::actionFailed, &guard, [&](const QString &a, const QString &) { failed << a; });
    QObject::connect(s, &RexSession::performRequested, &guard,
                     [&](const QString &call, const QString &action, const QJsonObject &args, TerminalWidget *) {
                         performCall = call;
                         performAction = action;
                         performArgs = args;
                     });
    QObject::connect(s, &RexSession::promptRequested, &guard, [&](const QString &call, const QJsonObject &p) {
        promptCall = call;
        prompt = p;
    });

    if (!check(waitFor([&] { return s->keymap().isLoaded(); }), QStringLiteral("a keymap")))
        return;
    const RexKeymap &km = s->keymap();
    const auto all = [](const QString &) { return true; };
    const RexKeymap::Binding *b = nullptr;
    check(km.lookup({}, {{"ctrl+shift+c"}}, all, &b) == RexKeymap::Result::Run && b->action == QLatin1String("pane.copy"),
          QStringLiteral("the client's default binding"));
    check(km.action(QStringLiteral("com.superlogical.terminal.clear")) != nullptr, QStringLiteral("block actions listed"));
    check(!km.action(QStringLiteral("client.tab.new")), QStringLiteral("the client's own actions aren't"));

    // A block action, and one there is none of.
    s->runAction(QStringLiteral("com.superlogical.terminal.clear"), {}, c.tabs.value(0)->activePane(), QStringLiteral("api"));
    check(waitFor([&] { return finished.contains(QStringLiteral("com.superlogical.terminal.clear")); }),
          QStringLiteral("a block action ran"));
    s->runAction(QStringLiteral("no.such.action"), {}, nullptr, QStringLiteral("api"));
    check(waitFor([&] { return failed.contains(QStringLiteral("no.such.action")); }), QStringLiteral("an unknown action fails"));

    // Another client asks this one to run its action.
    {
        QJsonObject result, err;
        bool done = false;
        server.control.request(QStringLiteral("client.perform"),
                               QJsonObject{{QStringLiteral("action"), QStringLiteral("client.tab.new")},
                                           {QStringLiteral("client_id"), s->clientId()},
                                           {QStringLiteral("args"), QJsonObject{{QStringLiteral("n"), 2}}}},
                               [&](const QJsonObject &p, const QJsonObject &e) {
                                   result = p;
                                   err = e;
                                   done = true;
                               });
        if (check(waitFor([&] { return !performCall.isEmpty(); }), QStringLiteral("client_perform_requested"))) {
            check(performAction == QLatin1String("client.tab.new") && performArgs.value("n").toInt() == 2,
                  QStringLiteral("the action and its arguments"));
            s->replyCall(std::exchange(performCall, QString()), QJsonObject{{QStringLiteral("made"), true}});
        }
        check(waitFor([&] { return done; }) && err.isEmpty()
                  && result.value("result").toObject().value("made").toBool()
                  && result.value("performed_by").toString() == s->clientId(),
              QStringLiteral("client.perform answered: %1 %2")
                  .arg(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)),
                       QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact))));
        // One this client didn't declare: refused by the server.
        server.call(QStringLiteral("client.perform"),
                    QJsonObject{{QStringLiteral("action"), QStringLiteral("client.nothing")},
                                {QStringLiteral("client_id"), s->clientId()}},
                    true);
        check(performCall.isEmpty(), QStringLiteral("an undeclared action isn't asked for"));
    }

    if (!km.action(QStringLiteral("ask")) || !km.action(QStringLiteral("via_client"))) {
        QTextStream(stdout) << "keymap: no test actions configured; actions skipped\n";
        return;
    }
    // The configuration's bindings: first, a sequence, an unbound default.
    check(km.lookup({}, {{"ctrl+shift+h"}}, all, &b) == RexKeymap::Result::Run && b->action == QLatin1String("hello"),
          QStringLiteral("a configured binding"));
    check(km.lookup({}, {{"ctrl+a"}}, all) == RexKeymap::Result::Prefix
              && km.lookup({}, {{"ctrl+a"}, {"c"}}, all) == RexKeymap::Result::Run,
          QStringLiteral("a configured sequence"));
    check(km.lookup({}, {{"ctrl+shift+t"}}, all) == RexKeymap::Result::Unbound, QStringLiteral("a default unbound"));

    finished.clear();
    results.clear();
    s->runAction(QStringLiteral("hello"), {}, nullptr, QStringLiteral("key"));
    check(waitFor([&] { return finished.contains(QStringLiteral("hello")); })
              && results.value(0).toString() == QStringLiteral("hi ") + s->clientId(),
          QStringLiteral("a Lua action's result"));

    // Prompts: answered, picked, cancelled.
    const auto answer = [&](const QString &action, const QJsonObject &reply, const QString &kind) -> QJsonValue {
        finished.clear();
        results.clear();
        promptCall.clear();
        s->runAction(action, {}, nullptr, QStringLiteral("palette"));
        if (!check(waitFor([&] { return !promptCall.isEmpty(); }), action + QStringLiteral(": a prompt")))
            return {};
        check(prompt.value("kind").toString() == kind, action + QStringLiteral(": a ") + kind + QStringLiteral(" prompt"));
        s->replyCall(promptCall, reply);
        waitFor([&] { return finished.contains(action); });
        return results.value(0);
    };
    check(answer(QStringLiteral("ask"), QJsonObject{{QStringLiteral("text"), QStringLiteral("bob")}}, QStringLiteral("input"))
              == QJsonValue(QStringLiteral("bob")),
          QStringLiteral("an answer"));
    check(prompt.value("value").toString() == QLatin1String("x"), QStringLiteral("the prompt's value"));
    check(answer(QStringLiteral("choose"), QJsonObject{{QStringLiteral("ids"), QJsonArray{QStringLiteral("b")}}},
                 QStringLiteral("pick"))
              == QJsonValue(QStringLiteral("b")),
          QStringLiteral("a pick"));
    check(prompt.value("items").toArray().size() == 2, QStringLiteral("the pick's items"));
    check(answer(QStringLiteral("ask"), QJsonObject{{QStringLiteral("cancelled"), true}}, QStringLiteral("input"))
              == QJsonValue(QStringLiteral("cancelled")),
          QStringLiteral("cancelled"));

    // An action that runs one of the client's.
    finished.clear();
    results.clear();
    s->runAction(QStringLiteral("via_client"), {}, nullptr, QStringLiteral("key"));
    if (check(waitFor([&] { return !performCall.isEmpty(); }), QStringLiteral("rex.perform asks the client"))) {
        check(performArgs.value("why").toString() == QLatin1String("test"), QStringLiteral("with its arguments"));
        s->replyCall(std::exchange(performCall, QString()), {},
                     QJsonObject{{QStringLiteral("code"), QStringLiteral("not_supported")},
                                 {QStringLiteral("message"), QStringLiteral("not here")}});
        check(waitFor([&] { return failed.contains(QStringLiteral("via_client")); }),
              QStringLiteral("the client's refusal fails the action"));
    }

    // The client's own scripts: key handlers, forwarded keys.
    if (TerminalWidget *pane = c.panes().value(0)) {
        QJsonObject reply, err;
        bool done = false;
        const int before = keymaps;
        s->setScripts(QJsonArray{QJsonObject{
                          {QStringLiteral("name"), QStringLiteral("keys.lua")},
                          {QStringLiteral("source"), QStringLiteral(
                               "rex.bind('ctrl+shift+u', function(ctx, key)\n"
                               "  rex.call('session.set_block_label', {session_id = ctx.session_id,\n"
                               "    block_id = ctx.block_id, label = 'forwarded ' .. key.key})\n"
                               "  return true end)\n"
                               "rex.bind('z', function() return false end)")}}},
                      [&](const QJsonObject &r, const QJsonObject &e) {
                          reply = r;
                          err = e;
                          done = true;
                      });
        waitFor([&] { return done; });
        if (err.value(QLatin1String("code")).toString() == QLatin1String("method_not_found")) {
            QTextStream(stdout) << "keymap: the server has no client.set_script; forwarded keys skipped\n";
        } else if (check(err.isEmpty(), QStringLiteral("client.set_script: ") + err.value(QLatin1String("message")).toString())) {
            check(reply.value(QLatin1String("bindings")).toArray()
                      == QJsonArray({QStringLiteral("ctrl+shift+u"), QStringLiteral("z")}),
                  QStringLiteral("set_script's keys"));
            check(waitFor([&] {
                      return keymaps > before && s->keymap().lookup({}, {{"ctrl+shift+u"}}, all, &b) == RexKeymap::Result::Run
                             && b->action == QLatin1String("client.key.forward");
                  }),
                  QStringLiteral("the handler's key in the keymap, forwarded"));
            const QString bid = s->blockId(pane);
            const auto label = [&] {
                return server.call(QStringLiteral("session.describe_block"),
                                   QJsonObject{{QStringLiteral("session_id"), s->sessionId()}, {QStringLiteral("block_id"), bid}})
                    .value(QLatin1String("block")).toObject().value(QLatin1String("label")).toString();
            };
            s->forwardKey(pane, RexKeymap::keyEvent(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier, QString()), {});
            check(waitFor([&] { return label() == QLatin1String("forwarded key_u"); }),
                  QStringLiteral("a forwarded key's handler ran, as this client"));
            // A handler that lets the key by: the server types it.
            type(pane, QStringLiteral("echo FWD_"));
            s->forwardKey(pane, RexKeymap::keyEvent(Qt::Key_Z, Qt::NoModifier, QStringLiteral("z")), {});
            type(pane, QStringLiteral("_END\r"));
            check(waitFor([&] { return shows(pane, QStringLiteral("FWD_z_END")) || shows(pane, QStringLiteral("FWD__END")); })
                      && !shows(pane, QStringLiteral("FWD__END")),
                  QStringLiteral("an unhandled forwarded key reaches the terminal"));
            s->setScripts({});
            check(waitFor([&] { return s->keymap().lookup({}, {{"ctrl+shift+u"}}, all) == RexKeymap::Result::Unbound; }),
                  QStringLiteral("scripts removed: the key unbound"));

            // The scripts file in the settings (as on attaching, or a change).
            QTemporaryDir dir;
            const QString file = dir.filePath(QStringLiteral("pj-keys.lua"));
            const auto writeLua = [&](const QByteArray &lua) {
                QFile f(file);
                check(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(lua) == lua.size(),
                      QStringLiteral("write ") + file);
            };
            writeLua("rex.bind('ctrl+shift+k', function() return true end)\n");
            settings.setRexScriptsFile(file);
            int outcomes = 0;
            bool lastOk = false;
            QString lastMessage;
            const auto outcome = [&](bool ok, const QString &message) {
                ++outcomes;
                lastOk = ok;
                lastMessage = message;
            };
            s->sendScripts(outcome);
            check(waitFor([&] { return s->keymap().lookup({}, {{"ctrl+shift+k"}}, all, &b) == RexKeymap::Result::Run
                                       && b->action == QLatin1String("client.key.forward"); }),
                  QStringLiteral("the settings' scripts file sent"));
            check(waitFor([&] { return outcomes == 1; }) && lastOk, QStringLiteral("sendScripts: ok"));
            writeLua("this is not lua");
            s->sendScripts(outcome);
            check(waitFor([&] { return failed.contains(QStringLiteral("pj-keys.lua")); })
                      && s->keymap().lookup({}, {{"ctrl+shift+k"}}, all) == RexKeymap::Result::Run,
                  QStringLiteral("a broken file is reported, the scripts before kept"));
            check(waitFor([&] { return outcomes == 2; }) && !lastOk && lastMessage.contains(QLatin1String("pj-keys.lua")),
                  QStringLiteral("sendScripts: the server's reason: ") + lastMessage);
            // An init.lua action isn't a client script, as in Rex.
            writeLua("rex.action{name = 'x', title = 'X', run = function() end}");
            s->sendScripts(outcome);
            check(waitFor([&] { return outcomes == 3; }) && !lastOk, QStringLiteral("rex.action refused: ") + lastMessage);
            settings.setRexScriptsFile(QString());
            s->sendScripts();
            check(waitFor([&] { return s->keymap().lookup({}, {{"ctrl+shift+k"}}, all) == RexKeymap::Result::Unbound; }),
                  QStringLiteral("no file: the scripts sent removed"));
        }
    }

    // The configuration reloaded: a new keymap.
    const QString path = km.configPath();
    if (path.isEmpty() || !path.startsWith(QDir::tempPath())) {
        QTextStream(stdout) << "keymap: configuration not in " << QDir::tempPath() << "; reload skipped\n";
        return;
    }
    // server.config_reload may be for the local socket only; a signal does
    // as well, to a server given by its PID.
    const qint64 pid = qEnvironmentVariableIntValue("PUDDLEJUMPER_TEST_REX_PID");
    const auto reload = [&]() {
        if (pid > 0)
            ::kill(pid_t(pid), SIGHUP);
        else
            server.call(QStringLiteral("server.config_reload"), {}, true);
    };
    if (pid <= 0) {
        QJsonObject err;
        bool done = false;
        server.control.request(QStringLiteral("server.config_reload"), {},
                               [&](const QJsonObject &, const QJsonObject &e) { err = e; done = true; });
        waitFor([&] { return done; });
        if (!err.isEmpty()) {
            QTextStream(stdout) << "keymap: " << err.value("message").toString()
                                << "; reload skipped (PUDDLEJUMPER_TEST_REX_PID=<server pid> sends SIGHUP)\n";
            return;
        }
    }
    QFile f(path);
    if (!check(f.open(QIODevice::ReadOnly), QStringLiteral("read ") + path))
        return;
    const QByteArray original = f.readAll();
    f.close();
    const auto write = [&](const QByteArray &data) {
        QFile w(path);
        check(w.open(QIODevice::WriteOnly | QIODevice::Truncate) && w.write(data) == data.size(),
              QStringLiteral("write ") + path);
    };
    const int before = keymaps;
    write(original + "\nrex.bind('ctrl+shift+y', 'hello')\n");
    reload();
    check(waitFor([&] { return keymaps > before; })
              && s->keymap().lookup({}, {{"ctrl+shift+y"}}, all) == RexKeymap::Result::Run,
          QStringLiteral("keymap_changed: fetched again"));
    write(original + "\nthis is not lua\n");
    reload();
    check(waitFor([&] { return keymaps > before + 1; }) && !s->keymap().diagnostics().isEmpty(),
          QStringLiteral("a broken configuration's diagnostics"));
    write(original);
    reload();
    check(waitFor([&] { return keymaps > before + 2; }) && s->keymap().diagnostics().isEmpty(),
          QStringLiteral("and fixed again"));
}

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

        // Its block is the other client's: shown at that client's size,
        // marked following, until it is used here.
        TerminalWidget *theirs = nullptr;
        for (TerminalWidget *p : second->panes())
            if (c.session->isFollowing(p))
                theirs = p;
        if (check(theirs && theirs->isFollowing(), QStringLiteral("a block made elsewhere is followed, and marked"))) {
            const QString bid = c.session->blockId(theirs);
            const QSize before = server.size(sid, bid);
            if (const QString shots = qEnvironmentVariable("PUDDLEJUMPER_TEST_SHOTS"); !shots.isEmpty()) {
                waitFor([] { return false; }, 300);
                second->grab().save(shots + QStringLiteral("/following.png"));
            }
            type(theirs, QStringLiteral("echo TAKEN\r"));
            check(waitFor([&] { return !c.session->isFollowing(theirs) && !theirs->isFollowing(); }),
                  QStringLiteral("typing takes it"));
            check(waitFor([&] { return server.size(sid, bid) != before; }),
                  QStringLiteral("and it takes this pane's size"));
            agrees(server, c, QStringLiteral("taken"));
        }
    }

    // ---- The session renamed, here and elsewhere; the active window.
    {
        QString told;
        QObject::connect(c.session, &RexSession::labelChanged, [&](const QString &l) { told = l; });
        c.session->renameSession(QStringLiteral("named here"));
        check(waitFor([&] { return server.view(sid).value(QLatin1String("label")).toString() == QLatin1String("named here"); }),
              QStringLiteral("renamed here: the server has it"));
        check(waitFor([&] { return told == QLatin1String("named here") && c.session->label() == told; }),
              QStringLiteral("and labelChanged told"));
        server.call(QStringLiteral("session.set_label"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("label"), QStringLiteral("named elsewhere")}});
        check(waitFor([&] { return told == QLatin1String("named elsewhere") && c.session->label() == told; }),
              QStringLiteral("renamed elsewhere: followed"));

        const auto activeLabel = [&] {
            const QJsonObject v = server.view(sid);
            const QString active = v.value(QLatin1String("active_window_id")).toString();
            for (const QJsonValue &wv : v.value(QLatin1String("windows")).toArray())
                if (wv.toObject().value(QLatin1String("window_id")).toString() == active)
                    return wv.toObject().value(QLatin1String("label")).toString();
            return QString();
        };
        c.session->windowShown(c.tabs.value(0));
        check(waitFor([&] { return activeLabel() == c.names.value(c.tabs.value(0)); }),
              QStringLiteral("the first tab shown: the server's active window"));
        c.session->windowShown(second);
        check(waitFor([&] { return activeLabel() == QLatin1String("renamed elsewhere"); }),
              QStringLiteral("the second tab shown: active again"));
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

    // ---- Two panes swapped: each block's pane now where the other's was.
    if (second && second->panes().size() == 2) {
        const QList<TerminalWidget *> two = second->panes();
        c.session->swapWithNext(two.first());
        check(waitFor([&] { return second->panes() == QList<TerminalWidget *>{two.last(), two.first()}; }),
              QStringLiteral("swapped: the same panes, the other way round"));
        agrees(server, c, QStringLiteral("swapped"));
        c.session->swapWithNext(two.first());
        check(waitFor([&] { return second->panes() == two; }), QStringLiteral("and back"));
        agrees(server, c, QStringLiteral("swapped back"));
    }

    // ---- A pane moved to another window, and back.
    if (second && second->panes().size() == 2 && c.tabs.value(0)) {
        TerminalTab *firstTab = c.tabs.value(0);
        TerminalWidget *mover = second->panes().last();
        const QString moverBlock = c.session->blockId(mover);
        c.session->movePane(mover, firstTab);
        check(waitFor([&] { return firstTab->panes().size() == 2 && second->panes().size() == 1; }),
              QStringLiteral("moved to the first window"));
        TerminalWidget *there = c.session->paneForBlock(moverBlock);
        check(there && firstTab->contains(there), QStringLiteral("its block in the first tab"));
        agrees(server, c, QStringLiteral("moved between windows"));
        c.session->movePane(there, second);
        check(waitFor([&] { return firstTab->panes().size() == 1 && second->panes().size() == 2; }),
              QStringLiteral("and back"));
        agrees(server, c, QStringLiteral("moved back"));
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
                // [x, y, w, h], as Rex sends it (older atlantis: {x, y, w, h}).
                const QJsonValue b = lo.value(QLatin1String("bounds"));
                const QJsonArray a = b.toArray();
                const QJsonObject o = b.toObject();
                out.append({lo.value(QLatin1String("layer_id")).toString(),
                            b.isArray() ? QRectF(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(),
                                                 a.at(3).toDouble())
                                        : QRectF(o.value(QLatin1String("x")).toDouble(), o.value(QLatin1String("y")).toDouble(),
                                                 o.value(QLatin1String("w")).toDouble(), o.value(QLatin1String("h")).toDouble())});
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
        // Pictures of the floating frame and a tiled pane's hover buttons, to
        // look at: PUDDLEJUMPER_TEST_SHOTS=dir.
        if (const QString shots = qEnvironmentVariable("PUDDLEJUMPER_TEST_SHOTS"); !shots.isEmpty()) {
            const auto settle = [](int ms) {
                QEventLoop loop;
                QTimer::singleShot(ms, &loop, &QEventLoop::quit);
                loop.exec();
            };
            settle(300);
            second->grab().save(shots + QStringLiteral("/float.png"));
            TerminalWidget *tiled = second->panes().first();
            QCursor::setPos(tiled->mapToGlobal(QPoint(60, tiled->height() - 60)));
            second->updateHoverControls();
            settle(200);
            second->grab().save(shots + QStringLiteral("/hover.png"));
        }

        // Two zooms (revision 21): the tiled pane zoomed stays under the
        // floating one, and keeps its zoom while the float has focus; a
        // zoomed float covers everything, until focus leaves it.
        {
            TerminalWidget *tiled = second->panes().first();
            const QString tiledBlock = c.session->blockId(tiled);
            const QString floatBlock = c.session->blockId(floater);
            const auto zoom = [&](const QString &block, bool on) {
                server.call(QStringLiteral("session.zoom_block"),
                            QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("block_id"), block},
                                        {QStringLiteral("zoom"), on}});
            };
            const auto focus = [&](const QString &block) {
                server.call(QStringLiteral("session.focus_block"),
                            QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("block_id"), block}});
            };
            zoom(tiledBlock, true);
            check(waitFor([&] { return c.session->isZoomed(tiled) && second->panes().size() == 2
                                       && c.session->isFloating(floater); }),
                  QStringLiteral("tiled zoom: the float still shown over it"));
            focus(floatBlock);
            check(waitFor([&] { return second->activePane() == floater; }), QStringLiteral("focus on the float"));
            check(c.session->isZoomed(tiled) && second->panes().size() == 2,
                  QStringLiteral("tiled zoom kept while the float has focus"));
            zoom(floatBlock, true);
            check(waitFor([&] { return second->panes() == QList<TerminalWidget *>{floater}; }),
                  QStringLiteral("a zoomed float covers everything"));
            focus(tiledBlock);
            check(waitFor([&] { return second->panes().size() == 2 && !c.session->isZoomed(floater); }),
                  QStringLiteral("focus off the float ends its zoom"));
            check(c.session->isZoomed(tiled), QStringLiteral("and the tiled zoom is still on"));
            agrees(server, c, QStringLiteral("two zooms"));
            zoom(tiledBlock, false);
            check(waitFor([&] { return !c.session->isZoomed(tiled); }), QStringLiteral("tiled zoom off"));
        }

        // Resized elsewhere.
        server.call(QStringLiteral("session.resize_layer"),
                    QJsonObject{{QStringLiteral("session_id"), sid}, {QStringLiteral("layer_id"), layer},
                                {QStringLiteral("bounds"), QJsonArray{0.0, 0.0, 0.4, 0.4}}});
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
        check(qAbs(moved.width() - 0.4) < 1e-9 && qAbs(moved.height() - 0.4) < 1e-9,
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
        check(waitFor([&] { return layersOf(label).value(0).second.width() > 0.45; }),
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
                                {QStringLiteral("bounds"), QJsonArray{0.5, 0.5, 0.45, 0.45}},
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

    // ---- Floated where it sits, and docked by where the float is: back
    // where it was if it hasn't moved, else beside the pane under it.
    {
        c.session->newWindow();
        TerminalTab *tab = waitFor([&] { return c.tabs.size() == 3; }) ? c.tabs.value(2) : nullptr;
        if (check(tab != nullptr, QStringLiteral("dock test: a window"))) {
            c.session->renameWindow(tab, QStringLiteral("dock test"));
            waitFor([&] { return c.names.value(tab) == QLatin1String("dock test"); });
            c.session->splitPane(tab->activePane(), Qt::Horizontal);
            waitFor([&] { return tab->panes().size() == 2; });
            c.session->splitPane(tab->panes().at(1), Qt::Vertical);
            waitFor([&] { return tab->panes().size() == 3; });
            // The tiled layout's shape: blocks, and splits' direction and ratio.
            const auto shape = [&]() {
                std::function<QString(const QJsonObject &)> walk = [&](const QJsonObject &n) -> QString {
                    if (n.contains(QLatin1String("block_id")))
                        return n.value(QLatin1String("block_id")).toString().right(4);
                    const QJsonObject sp = n.value(QLatin1String("split")).toObject();
                    return QStringLiteral("%1/%2(%3,%4)")
                        .arg(sp.value(QLatin1String("direction")).toString().left(1))
                        .arg(sp.value(QLatin1String("ratio")).toDouble(), 0, 'f', 2)
                        .arg(walk(sp.value(QLatin1String("before")).toObject()),
                             walk(sp.value(QLatin1String("after")).toObject()));
                };
                for (const QJsonValue &wv : server.view(sid).value(QLatin1String("windows")).toArray())
                    if (wv.toObject().value(QLatin1String("label")).toString() == QLatin1String("dock test"))
                        for (const QJsonValue &lv : wv.toObject().value(QLatin1String("layers")).toArray())
                            if (lv.toObject().value(QLatin1String("kind")).toString() == QLatin1String("tiled"))
                                return walk(lv.toObject().value(QLatin1String("layout")).toObject());
                return QString();
            };
            const QString before = shape();
            TerminalWidget *third = tab->panes().value(2);
            const QRect area = tab->layoutRect();
            const QRect at(third->mapTo(tab, QPoint(0, 0)) - area.topLeft(), third->size());
            c.session->floatPane(third);
            check(waitFor([&] { return c.session->isFloating(third); }), QStringLiteral("dock test: floated"));
            const QList<QPair<QString, QRectF>> layers = layersOf(QStringLiteral("dock test"));
            const QRectF b = layers.value(0).second;
            check(layers.size() == 1 && qAbs(b.x() - at.x() / double(area.width())) < 0.02
                      && qAbs(b.y() - at.y() / double(area.height())) < 0.02
                      && qAbs(b.width() - at.width() / double(area.width())) < 0.02,
                  QStringLiteral("floated where it sat (%1,%2 %3x%4)").arg(b.x()).arg(b.y()).arg(b.width()).arg(b.height()));
            c.session->dockPane(third);
            check(waitFor([&] { return !c.session->isFloating(third) && shape() == before; }),
                  QStringLiteral("docked straight back: the layout as it was (%1, was %2)").arg(shape(), before));

            // Floated again and moved to the left edge: docked left of the
            // left pane.
            const QString leftId = c.session->blockId(tab->panes().value(0));
            c.session->floatPane(third);
            waitFor([&] { return c.session->isFloating(third) && layersOf(QStringLiteral("dock test")).size() == 1; });
            server.call(QStringLiteral("session.resize_layer"),
                        QJsonObject{{QStringLiteral("session_id"), sid},
                                    {QStringLiteral("layer_id"), layersOf(QStringLiteral("dock test")).value(0).first},
                                    {QStringLiteral("bounds"), QJsonArray{0.0, 0.3, 0.2, 0.4}}});
            waitFor([&] {
                return qAbs(layersOf(QStringLiteral("dock test")).value(0).second.x()) < 0.01
                       && c.session->isFloating(third) && third->isVisible();
            });
            waitFor([] { return false; }, 300);  // the view followed here
            c.session->dockPane(third);
            const QString moved = c.session->blockId(third).right(4), left = leftId.right(4);
            check(waitFor([&] { return shape().startsWith(QStringLiteral("h/")) && shape().contains(QStringLiteral("(%1,%2)").arg(moved, left)); }),
                  QStringLiteral("docked where the float was: left of the left pane (%1)").arg(shape()));
            agrees(server, c, QStringLiteral("dock test"));
            c.session->closeWindow(tab);
            waitFor([&] { return c.tabs.size() == 2; });
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

    // ---- Keymaps and actions.
    keymapTests(server, c, settings);

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
