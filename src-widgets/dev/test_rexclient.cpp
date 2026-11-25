/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexclient: RexControl and RexTerminalConnection against a running
 * Rex-compatible server, with qtermwidget's emulator (no window) showing the
 * terminal. Each step ends with the emulator's text, history included,
 * compared with the server's own (com.superlogical.terminal.format):
 *
 *   a session with /bin/sh; output; the stream dropped and resumed while
 *   output goes on; a fresh stream (a new snapshot); a resize; the program
 *   exiting.
 *
 * It creates one session and destroys it. Run against a server on loopback,
 * e.g. tuxosaurus --listen 127.0.0.1:51433:
 *   ./build/test-rexclient http://127.0.0.1:51433
 */

#include "ghosttyreplay.h"
#include "qtermprobe.h"
#include "rexclient.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <QJsonArray>
#include <QPointer>
#include <QTextStream>

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

struct Call
{
    QJsonObject payload, error;
    bool done = false;
};

Call call(RexControl &c, const QString &method, const QJsonObject &payload)
{
    auto result = std::make_shared<Call>();
    c.request(method, payload, [result](const QJsonObject &p, const QJsonObject &e) {
        result->payload = p;
        result->error = e;
        result->done = true;
    });
    if (!waitFor([&] { return result->done; }))
        result->error = QJsonObject{{QStringLiteral("code"), QStringLiteral("timeout")}};
    if (!result->error.isEmpty())
        check(false, method + QStringLiteral(": ") + QString::fromUtf8(QJsonDocument(result->error).toJson(QJsonDocument::Compact)));
    return *result;
}

// The terminal as the client shows it.
struct Client
{
    std::unique_ptr<QtermProbe> emu;
    int snapshots = 0;
    QList<QJsonObject> markers;
    int interruptions = 0, resumes = 0;
    QString failure;

    void attach(RexTerminalConnection &t)
    {
        QPointer<RexTerminalConnection> conn(&t);
        QObject::connect(&t, &RexTerminalConnection::snapshot, [this](const GhosttySnapshot::Snapshot &s) {
            ++snapshots;
            emu = std::make_unique<QtermProbe>(s.terminal.columns, s.terminal.rows);
            emu->feed(GhosttySnapshot::replay(s));
        });
        QObject::connect(&t, &RexTerminalConnection::output, [this](const QByteArray &b) {
            if (emu)
                emu->feed(b);
        });
        QObject::connect(&t, &RexTerminalConnection::marker, [this, conn](const QJsonObject &m) {
            markers << m;
            if (emu && m.value(QLatin1String("type")).toString() == QLatin1String("resize")) {
                const int cols = m.value(QLatin1String("cols")).toInt();
                const int rows = m.value(QLatin1String("rows")).toInt();
                if (cols > 0 && rows > 0) {
                    // A new width: Ghostty rewraps, qtermwidget would cut,
                    // so take the server's snapshot instead.
                    if (cols != emu->screen(0)->getColumns() && conn)
                        conn->resync();
                    emu->setImageSize(rows, cols);
                }
            }
        });
        QObject::connect(&t, &RexTerminalConnection::interrupted, [this]() { ++interruptions; });
        QObject::connect(&t, &RexTerminalConnection::resumed, [this]() { ++resumes; });
        QObject::connect(&t, &RexTerminalConnection::failed, [this](const QString &why) { failure = why; });
    }

    QStringList lines() const
    {
        if (!emu)
            return {};
        QStringList l = QtermProbeLines::screenLines(emu->screen(0));
        while (!l.isEmpty() && l.last().isEmpty())
            l.removeLast();
        return l;
    }
    bool shows(const QString &text) const { return lines().join(QLatin1Char('\n')).contains(text); }
};

QStringList serverLines(RexControl &c, const QJsonObject &target)
{
    QJsonObject args = target;
    args.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("trim"), true}});
    const Call r = call(c, QStringLiteral("com.superlogical.terminal.format"), args);
    QStringList l = r.payload.value(QLatin1String("content")).toString().split(QLatin1Char('\n'));
    while (!l.isEmpty() && l.last().isEmpty())
        l.removeLast();
    return l;
}

void compare(RexControl &c, const QJsonObject &target, const Client &client, const QString &when)
{
    // Output may still be on its way: wait until they agree, or time out.
    QStringList want, got;
    const bool same = waitFor([&] {
        want = serverLines(c, target);
        got = client.lines();
        return want == got;
    }, 5000);
    if (!check(same, when + QStringLiteral(": the client shows what the server has"))) {
        QTextStream err(stderr);
        err << "    client:\n";
        for (const QString &l : got)
            err << "      |" << l << "|\n";
        err << "    server:\n";
        for (const QString &l : want)
            err << "      |" << l << "|\n";
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString server = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("PJ_REX_SERVER");
    if (server.isEmpty()) {
        QTextStream(stdout) << "usage: test-rexclient http://host:port (or PJ_REX_SERVER); skipped\n";
        return 0;
    }

    RexControl control;
    control.open(QUrl(server));
    if (!check(waitFor([&] { return control.isReady() || !control.lastError().isEmpty(); }) && control.isReady(),
               QStringLiteral("control connection: ") + control.lastError()))
        return 1;
    check(!control.clientId().isEmpty(), QStringLiteral("a client id"));

    const QJsonObject block{{QStringLiteral("flavor"), QStringLiteral("com.superlogical.terminal.shell")},
                            {QStringLiteral("options"),
                             QJsonObject{{QStringLiteral("command"), QJsonArray{QStringLiteral("/bin/sh")}},
                                         {QStringLiteral("login"), false}}}};
    const Call created = call(control, QStringLiteral("session.create"),
                              QJsonObject{{QStringLiteral("label"), QStringLiteral("puddlejumper test")},
                                          {QStringLiteral("initial_windows"),
                                           QJsonArray{QJsonObject{{QStringLiteral("layout"),
                                                                   QJsonObject{{QStringLiteral("block"), block}}}}}}});
    const QString sid = created.payload.value(QLatin1String("session_id")).toString();
    if (!check(!sid.isEmpty(), QStringLiteral("a session")))
        return 1;
    const Call attached = call(control, QStringLiteral("session.attach"), QJsonObject{{QStringLiteral("session_id"), sid}});
    const QString bid = attached.payload.value(QLatin1String("view")).toObject()
                            .value(QLatin1String("focused_window")).toObject()
                            .value(QLatin1String("focused_block_id")).toString();
    check(!bid.isEmpty(), QStringLiteral("a block"));
    const QJsonObject target{{QStringLiteral("session_id"), sid}, {QStringLiteral("block_id"), bid}};
    auto blockCall = [&](const QString &method, const QJsonObject &args) {
        QJsonObject p = target;
        p.insert(QStringLiteral("args"), args);
        return call(control, method, p);
    };
    auto type = [&](const QString &text) {
        blockCall(QStringLiteral("com.superlogical.terminal.write"), QJsonObject{{QStringLiteral("data"), text}});
    };
    blockCall(QStringLiteral("com.superlogical.terminal.resize"),
              QJsonObject{{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}});

    // Some history before the stream opens.
    type(QStringLiteral("i=1; while [ $i -le 40 ]; do echo \"before $i\"; i=$((i+1)); done\r"));
    type(QStringLiteral("printf '\\033[1;32mgreen\\033[0m \\344\\270\\255\\346\\226\\207\\n'\r"));
    QThread::msleep(300);

    Client client;
    RexTerminalConnection terminal(&control, sid, bid);
    client.attach(terminal);
    terminal.open();
    check(waitFor([&] { return client.snapshots == 1; }), QStringLiteral("a snapshot: ") + client.failure);
    check(!client.markers.isEmpty() && client.markers.first().value(QLatin1String("type")).toString() == QLatin1String("resize"),
          QStringLiteral("a resize marker first"));
    compare(control, target, client, QStringLiteral("snapshot"));

    type(QStringLiteral("echo REX_ONE\r"));
    check(waitFor([&] { return client.shows(QStringLiteral("\nREX_ONE")); }), QStringLiteral("live output"));
    compare(control, target, client, QStringLiteral("live output"));

    // Dropped while output goes on: resumed, nothing lost or doubled.
    terminal.dropForTest();
    type(QStringLiteral("i=1; while [ $i -le 30 ]; do echo \"while away $i\"; i=$((i+1)); done; echo REX_TWO\r"));
    check(waitFor([&] { return client.resumes == 1 && client.shows(QStringLiteral("\nREX_TWO")); }),
          QStringLiteral("resumed: ") + client.failure);
    check(client.interruptions == 1 && client.snapshots == 1, QStringLiteral("resumed, not restarted"));
    compare(control, target, client, QStringLiteral("after resuming"));

    // A fresh stream: a new snapshot, the emulator rebuilt from it.
    terminal.close();
    terminal.open();
    check(waitFor([&] { return client.snapshots == 2; }), QStringLiteral("a second snapshot: ") + client.failure);
    compare(control, target, client, QStringLiteral("fresh stream"));

    // Resized: a marker; the width changed, so a fresh snapshot.
    const int markersBefore = int(client.markers.size());
    blockCall(QStringLiteral("com.superlogical.terminal.resize"),
              QJsonObject{{QStringLiteral("columns"), 70}, {QStringLiteral("rows"), 20}});
    check(waitFor([&] {
              return client.markers.size() > markersBefore
                  && client.markers.last().value(QLatin1String("cols")).toInt() == 70;
          }),
          QStringLiteral("a resize marker"));
    type(QStringLiteral("echo REX_THREE\r"));
    check(waitFor([&] { return client.shows(QStringLiteral("\nREX_THREE")); }), QStringLiteral("output after resize"));
    check(client.snapshots == 3, QStringLiteral("a snapshot after the width changed"));
    check(client.emu && client.emu->screen(0)->getColumns() == 70 && client.emu->screen(0)->getLines() == 20,
          QStringLiteral("the emulator is 70x20"));
    compare(control, target, client, QStringLiteral("after resize"));

    // The program exits: the control connection says so (a child_exited
    // block event); the server then closes the block, and the stream may
    // end before its child_exited marker goes out.
    QList<QJsonObject> events;
    QObject::connect(&control, &RexControl::event, [&](const QJsonObject &e) { events << e; });
    type(QStringLiteral("exit 3\r"));
    QJsonObject exited;
    check(waitFor([&] {
              for (const QJsonObject &e : events) {
                  const QByteArray json = QJsonDocument(e).toJson(QJsonDocument::Compact);
                  if (json.contains("\"child_exited\"")) {
                      exited = e;
                      return true;
                  }
              }
              return false;
          }),
          QStringLiteral("a child_exited event"));
    QTextStream(stdout) << "exit event: " << QJsonDocument(exited).toJson(QJsonDocument::Compact) << "\n";
    check(QJsonDocument(exited).toJson(QJsonDocument::Compact).contains("\"exit_code\":3"), QStringLiteral("exit code 3"));
    waitFor([&] { return !client.failure.isEmpty(); }, 3000);
    bool marker = false;
    for (const QJsonObject &m : client.markers)
        marker |= m.value(QLatin1String("type")).toString() == QLatin1String("child_exited");
    QTextStream(stdout) << "exit marker on the stream: " << (marker ? "yes" : "no")
                        << "; stream: " << (client.failure.isEmpty() ? QStringLiteral("open") : client.failure) << "\n";

    terminal.close();
    call(control, QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), sid}});
    control.close();

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
