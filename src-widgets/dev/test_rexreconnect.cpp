/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexreconnect: a Rex tab whose server goes away. It runs an atlantis
 * of its own (loopback port 51499, a temporary state directory and HOME),
 * and stops and starts it:
 *
 *   the server stops under an attached session: the tab stays, under the
 *   offline overlay, showing what it showed; the server starts again (its
 *   sessions restored): the same tab attaches again, the overlay goes, and
 *   typing works;
 *   a restore with the server away: placeholder tabs named as the saved
 *   windows, waiting; the server starts: they become the windows;
 *   a restore of a session the server no longer has: ended, no tabs;
 *   Close Tabs on the overlay: ended;
 *   opening (not restoring) with the server away: ended at once, as before.
 *
 *   ./build/test-rexreconnect /path/to/atlantis   (or PJ_ATLANTIS)
 */

#include "qtermprobe.h"
#include "rexclient.h"
#include "rexoffline.h"
#include "rexsession.h"
#include "settings.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include "ScreenWindow.h"
#include "TerminalDisplay.h"

namespace {

int failures = 0;
int checks = 0;
const QUrl kServer(QStringLiteral("http://127.0.0.1:51499"));

bool check(bool ok, const QString &what)
{
    ++checks;
    QTextStream(ok ? stdout : stderr) << (ok ? "ok    " : "FAIL  ") << what << "\n";
    if (!ok)
        ++failures;
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

QString paneText(TerminalWidget *pane)
{
    auto *d = pane ? pane->findChild<Konsole::TerminalDisplay *>() : nullptr;
    if (!d || !d->screenWindow())
        return {};
    return QtermProbeLines::screenLines(d->screenWindow()->screen()).join(QLatin1Char('\n'));
}

RexOfflineOverlay *overlayOf(TerminalTab *tab)
{
    if (!tab)
        return nullptr;
    auto *o = tab->findChild<RexOfflineOverlay *>(QString(), Qt::FindDirectChildrenOnly);
    return o && o->isVisible() ? o : nullptr;
}

// The test's atlantis.
struct Atlantis
{
    QString program;
    QTemporaryDir dir;
    QProcess process;

    bool start()
    {
        process.setProgram(program);
        process.setArguments({QStringLiteral("--listen"), QStringLiteral("127.0.0.1:51499"), QStringLiteral("--socket"),
                              QString(), QStringLiteral("--state-dir"), dir.path() + QStringLiteral("/state"),
                              QStringLiteral("--shell"), QStringLiteral("/bin/sh")});
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), dir.path());
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), dir.path() + QStringLiteral("/config"));
        env.remove(QStringLiteral("REX_CONFIG"));
        process.setProcessEnvironment(env);
        process.setStandardOutputFile(dir.path() + QStringLiteral("/server.log"), QIODevice::Append);
        process.setStandardErrorFile(dir.path() + QStringLiteral("/server.log"), QIODevice::Append);
        process.start();
        if (!process.waitForStarted(5000))
            return false;
        return waitFor([] {
            RexControl c;
            c.open(kServer);
            waitFor([&] { return c.isReady() || !c.lastError().isEmpty(); }, 500);
            return c.isReady();
        }, 10000);
    }

    // SIGTERM: it saves its sessions for the next start.
    void stop()
    {
        process.terminate();
        if (!process.waitForFinished(10000)) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
};

struct Client
{
    RexSession *session = nullptr;
    QList<QPointer<TerminalTab>> tabs;
    QHash<TerminalTab *, QString> names;
    QString ended;
    bool hasEnded = false;

    void open(const QString &sid, const Settings &settings, const TerminalProfile &profile,
              const QList<RexSession::SavedWindow> *restoring = nullptr, const QString &label = QString())
    {
        session = new RexSession(kServer, sid, settings, profile);
        if (restoring)
            session->setRestoring(*restoring, label);
        QObject::connect(session, &RexSession::tabAdded, [this](TerminalTab *tab, const QString &name) {
            tabs << tab;
            names.insert(tab, name);
            tab->resize(900, 520);
            tab->show();
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

    TerminalWidget *pane() const { return tabs.isEmpty() || !tabs.first() ? nullptr : tabs.first()->activePane(); }
};

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    Atlantis server;
    server.program = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("PJ_ATLANTIS");
    if (server.program.isEmpty()) {
        QTextStream(stdout) << "usage: test-rexreconnect /path/to/atlantis (or PJ_ATLANTIS); skipped\n";
        return 0;
    }
    {
        RexControl c;   // not someone else's server on the port
        c.open(kServer);
        waitFor([&] { return c.isReady() || !c.lastError().isEmpty(); }, 1500);
        if (c.isReady()) {
            QTextStream(stderr) << "something already answers on 127.0.0.1:51499; not testing\n";
            return 1;
        }
    }
    if (!check(server.start(), QStringLiteral("the test's atlantis started")))
        return 1;
    Settings settings;
    const TerminalProfile profile;
    const QString shots = qEnvironmentVariable("PUDDLEJUMPER_TEST_SHOTS");

    // ---- The server stops under an attached session, and comes back.
    Client c;
    c.open(QString(), settings, profile);
    if (!check(waitFor([&] { return c.tabs.size() == 1 || c.hasEnded; }) && c.tabs.size() == 1,
               QStringLiteral("a session, a tab ") + c.ended))
        return 1;
    TerminalTab *tab = c.tabs.first();
    waitFor([&] { return paneText(c.pane()).contains(QLatin1Char('$')); }, 5000);
    c.pane()->sendText(QStringLiteral("echo BEFORE_$((40+2))\r"));
    check(waitFor([&] { return paneText(c.pane()).contains(QStringLiteral("BEFORE_42")); }), QStringLiteral("typed into"));
    const QString sid = c.session->sessionId();

    server.stop();
    check(waitFor([&] { return c.session->isOffline(); }), QStringLiteral("server stopped: offline"));
    check(!c.hasEnded && c.tabs.size() == 1 && c.tabs.first() == tab, QStringLiteral("the tab stays"));
    check(waitFor([&] { return overlayOf(tab) != nullptr; }, 2000), QStringLiteral("the overlay is over it"));
    check(paneText(c.pane()).contains(QStringLiteral("BEFORE_42")), QStringLiteral("the pane still shows what it did"));
    check(c.session->shouldRestore(), QStringLiteral("saved with the window while offline"));
    waitFor([] { return false; }, 1500);   // a try or two fails meanwhile
    if (!shots.isEmpty())
        tab->grab().save(shots + QStringLiteral("/rex-offline.png"));

    if (!check(server.start(), QStringLiteral("the server started again")))
        return 1;
    check(waitFor([&] { return !c.session->isOffline() && c.session->isAttached(); }, 40000),
          QStringLiteral("attached again"));
    check(!c.hasEnded && c.tabs.size() == 1 && c.tabs.first() == tab, QStringLiteral("the same tab"));
    check(waitFor([&] { return overlayOf(tab) == nullptr; }, 2000), QStringLiteral("the overlay is gone"));
    check(waitFor([&] { return paneText(c.pane()).contains(QStringLiteral("restored: the server restarted")); }),
          QStringLiteral("the pane shows the restored terminal"));
    c.pane()->sendText(QStringLiteral("echo AFTER_$((3+4))\r"));
    check(waitFor([&] { return paneText(c.pane()).contains(QStringLiteral("AFTER_7")); }),
          QStringLiteral("typing works again"));

    // ---- A restore with the server away: placeholders, then the windows.
    const QList<RexSession::SavedWindow> windows = c.session->savedWindows();
    const QString label = c.session->label();
    check(windows.size() == 1 && windows.first().id.size() > 0, QStringLiteral("its window, to save"));
    delete c.session;   // as the app quitting: the session stays on the server
    for (auto &t : c.tabs)
        delete t.data();
    waitFor([] { return false; }, 300);
    server.stop();

    Client d;
    d.open(sid, settings, profile, &windows, label);
    check(waitFor([&] { return d.session->isOffline(); }), QStringLiteral("restore, server away: offline"));
    check(!d.hasEnded && d.tabs.size() == 1 && d.names.value(d.tabs.value(0)) == windows.first().label,
          QStringLiteral("a placeholder tab, named as the window (%1)").arg(d.names.value(d.tabs.value(0))));
    TerminalTab *placeholder = d.tabs.value(0);
    check(waitFor([&] { return overlayOf(placeholder) != nullptr; }, 2000), QStringLiteral("waiting under the overlay"));
    check(d.session->shouldRestore() && d.session->savedWindows().size() == 1,
          QStringLiteral("still saved, with its window"));
    if (!check(server.start(), QStringLiteral("the server started")))
        return 1;
    check(waitFor([&] { return d.session->isAttached(); }, 40000), QStringLiteral("attached"));
    check(d.tabs.size() == 1 && d.tabs.first() == placeholder, QStringLiteral("the placeholder became the window"));
    check(waitFor([&] { return paneText(d.pane()).contains(QStringLiteral("AFTER_7")); }),
          QStringLiteral("with its terminal"));
    check(waitFor([&] { return overlayOf(placeholder) == nullptr; }, 2000), QStringLiteral("no overlay"));

    // ---- A restore of a session the server doesn't have.
    Client e;
    const QList<RexSession::SavedWindow> none;
    e.open(QStringLiteral("no-such-session"), settings, profile, &none, QStringLiteral("gone"));
    waitFor([&] { return e.hasEnded; });
    check(e.hasEnded && e.tabs.isEmpty() && e.ended.contains(QStringLiteral("no longer")),
          QStringLiteral("a session that's gone: ended, no tabs (%1)").arg(e.ended));

    // ---- Close Tabs, while waiting.
    server.stop();
    check(waitFor([&] { return d.session->isOffline(); }), QStringLiteral("offline again"));
    waitFor([&] { return overlayOf(d.tabs.value(0)) != nullptr; }, 2000);
    QPushButton *close = nullptr;
    if (RexOfflineOverlay *o = overlayOf(d.tabs.value(0)))
        for (QPushButton *b : o->findChildren<QPushButton *>())
            if (b->text().contains(QStringLiteral("Close")))
                close = b;
    if (check(close != nullptr, QStringLiteral("Close Tabs")))
        close->click();
    waitFor([&] { return d.hasEnded; });
    check(d.hasEnded && d.tabs.isEmpty(), QStringLiteral("closed: ended (%1)").arg(d.ended));

    // ---- Opening (not restoring) with the server away: ended, as before.
    Client f;
    f.open(sid, settings, profile);
    waitFor([&] { return f.hasEnded; });
    check(f.hasEnded && f.tabs.isEmpty(),
          QStringLiteral("opening with no server: ended (%1)").arg(f.ended));

    // Leave nothing behind.
    if (server.start()) {
        RexControl ctl;
        ctl.open(kServer);
        bool done = false;
        ctl.request(QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), sid}},
                    [&](const QJsonObject &, const QJsonObject &) { done = true; });
        waitFor([&] { return done; }, 5000);
        server.stop();
    }
    QTextStream(stdout) << (failures ? "FAILED: " : "ok: ") << checks - failures << "/" << checks << " checks\n";
    return failures ? 1 : 0;
}
