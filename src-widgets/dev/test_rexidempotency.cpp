/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexidempotency: creating things on a Rex server over a connection
 * that drops before the response (API revision 26, idempotency keys). A
 * proxy of its own stands between the client and the server and cuts the
 * connection when the server's response passes (the server's frames aren't
 * masked, so it can be seen in them):
 *
 *   new_window, cut: sent again with its key after the reconnect; the
 *   server answers with the first call's window, and the session has one
 *   new window, not two;
 *   at revision 25 (no keys): answered "disconnected", not sent again; the
 *   window was made, as the view shows;
 *   a RexSession whose session.create is cut: it connects again, gets the
 *   session, attaches, and the server has one new session.
 *
 *   a reconnect that reaches another server (with a second one named):
 *   answered "disconnected", not sent again;
 *
 *   ./build/test-rexidempotency SERVER [OTHER-SERVER]
 *   (each http://127.0.0.1:PORT or unix:///path/rex.sock)
 *
 * Never against a server with sessions of yours: it creates and destroys
 * sessions.
 */

#include "rexclient.h"
#include "rexsession.h"
#include "settings.h"
#include "terminaltab.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>

namespace {

int failures = 0;
int checks = 0;

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

// Forwards loopback TCP to the server (TCP or its Unix socket). Armed, it
// cuts the connection that carries the next response to the client, and
// keeps what it cut.
class Proxy : public QObject
{
public:
    explicit Proxy(const QUrl &target)
        : m_target(target)
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *down = m_server.nextPendingConnection())
                pipe(down);
        });
    }

    void retarget(const QUrl &target) { m_target = target; }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }
    // Cut at the next response with `marker` in it.
    void arm(const QString &marker)
    {
        m_armed = true;
        m_marker = QRegularExpression(QStringLiteral("\"%1\"").arg(marker));
        m_cut.clear();
    }
    bool armed() const { return m_armed; }
    QByteArray cut() const { return m_cut; }

private:
    void pipe(QTcpSocket *down)
    {
        QIODevice *up = nullptr;
        if (m_target.scheme() == QLatin1String("unix")) {
            auto *s = new QLocalSocket(down);
            s->connectToServer(m_target.path());
            connect(s, &QLocalSocket::disconnected, down, &QTcpSocket::disconnectFromHost);
            up = s;
        } else {
            auto *s = new QTcpSocket(down);
            s->connectToHost(m_target.host(), m_target.port());
            connect(s, &QTcpSocket::disconnected, down, &QTcpSocket::disconnectFromHost);
            up = s;
        }
        connect(down, &QTcpSocket::disconnected, down, &QObject::deleteLater);
        connect(down, &QTcpSocket::readyRead, up, [down, up]() { up->write(down->readAll()); });
        connect(up, &QIODevice::readyRead, down, [this, down, up]() {
            const QByteArray data = up->readAll();
            static const QRegularExpression response(QStringLiteral("\"type\"\\s*:\\s*\"response\""));
            const QString text = QString::fromLatin1(data);
            if (m_armed && response.match(text).hasMatch() && m_marker.match(text).hasMatch()) {
                m_armed = false;
                m_cut = data;
                down->abort();
                return;
            }
            down->write(data);
        });
    }

    QUrl m_target;
    QTcpServer m_server;
    bool m_armed = false;
    QRegularExpression m_marker;
    QByteArray m_cut;
};

struct Call
{
    bool done = false;
    QJsonObject payload, error;
};

Call call(RexControl &c, const QString &method, const QJsonObject &payload, bool once = false)
{
    Call r;
    auto reply = [&r](const QJsonObject &p, const QJsonObject &e) { r = {true, p, e}; };
    if (once)
        c.requestOnce(method, payload, reply);
    else
        c.request(method, payload, reply);
    waitFor([&] { return r.done; });
    return r;
}

QJsonObject shell()
{
    return QJsonObject{{QStringLiteral("block"),
                        QJsonObject{{QStringLiteral("flavor"), QStringLiteral("com.superlogical.terminal.shell")}}}};
}

int windows(RexControl &c, const QString &sid)
{
    return call(c, QStringLiteral("session.view"), {{QStringLiteral("session_id"), sid}})
        .payload.value(QLatin1String("windows"))
        .toArray()
        .size();
}

QStringList sessions(RexControl &c)
{
    QStringList out;
    for (const QJsonValue &v : call(c, QStringLiteral("session.list"), {}).payload.value(QLatin1String("sessions")).toArray())
        out << v.toObject().value(QLatin1String("session_id")).toString();
    return out;
}

// A connection through the proxy that connects again when it drops, as a
// Rex tab does.
struct Reconnecting
{
    RexControl control;
    QUrl url;
    QString sid;   // attached again after a reconnect, before anything is sent again
    int drops = 0;

    explicit Reconnecting(const QUrl &u, int revision = RexControl::kApiRevision)
        : url(u)
    {
        control.setAskedRevision(revision);
        QObject::connect(&control, &RexControl::closed, &control, [this](const QString &) {
            ++drops;
            control.open(url);
            if (!sid.isEmpty())
                control.request(QStringLiteral("session.attach"), {{QStringLiteral("session_id"), sid}});
        });
        control.open(url);
        waitFor([&] { return control.isReady(); });
    }

    void attach(const QString &session)
    {
        sid = session;
        call(control, QStringLiteral("session.attach"), {{QStringLiteral("session_id"), sid}});
    }
};

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (argc < 2) {
        QTextStream(stdout) << "usage: test-rexidempotency http://127.0.0.1:PORT | unix:///path; skipped\n";
        return 0;
    }
    const QUrl server(QString::fromLocal8Bit(argv[1]));
    Proxy proxy(server);

    RexControl direct;
    direct.open(server);
    if (!check(waitFor([&] { return direct.isReady(); }), QStringLiteral("connected to %1").arg(server.toString())))
        return 1;
    const bool keys = direct.apiRevision() >= RexControl::kIdempotencyRevision;
    QTextStream(stdout) << "server speaks API revision " << direct.apiRevision() << "\n";

    // ---- new_window, cut before its response: sent again with the key.
    QString sid;
    {
        Reconnecting r(proxy.url());
        Call made = call(r.control, QStringLiteral("session.create"),
                         {{QStringLiteral("initial_windows"), QJsonArray{QJsonObject{{QStringLiteral("layout"), shell()}}}}},
                         true);
        sid = made.payload.value(QLatin1String("session_id")).toString();
        check(made.error.isEmpty() && !sid.isEmpty(), QStringLiteral("session.create through the proxy"));
        r.attach(sid);
        call(direct, QStringLiteral("session.attach"), {{QStringLiteral("session_id"), sid}});
        if (keys) {
            proxy.arm(QStringLiteral("window_id"));
            Call w = call(r.control, QStringLiteral("session.new_window"),
                          {{QStringLiteral("session_id"), sid}, {QStringLiteral("layout"), shell()}}, true);
            const QString first =
                QRegularExpression(QStringLiteral("\"window_id\"\\s*:\\s*\"([^\"]+)\"")).match(QString::fromUtf8(proxy.cut())).captured(1);
            check(r.drops == 1 && !first.isEmpty(), QStringLiteral("the connection was cut at the response (%1)").arg(first));
            check(w.error.isEmpty(), QStringLiteral("new_window answered after the reconnect ") +
                                         QString::fromUtf8(QJsonDocument(w.error).toJson(QJsonDocument::Compact)));
            check(w.payload.value(QLatin1String("window_id")).toString() == first,
                  QStringLiteral("with the first call's window"));
            const int n = windows(direct, sid);
            check(n == 2, QStringLiteral("one new window, not two (%1)").arg(n));
            check(!r.control.hasRetries(), QStringLiteral("nothing left to send again"));
        }
    }

    // ---- Without keys (revision 25): not sent again.
    {
        Reconnecting r(proxy.url(), 25);
        check(r.control.apiRevision() == 25, QStringLiteral("revision 25 asked for"));
        r.attach(sid);
        const int before = windows(direct, sid);
        proxy.arm(QStringLiteral("window_id"));
        Call w = call(r.control, QStringLiteral("session.new_window"),
                      {{QStringLiteral("session_id"), sid}, {QStringLiteral("layout"), shell()}}, true);
        check(w.error.value(QLatin1String("code")).toString() == QLatin1String("disconnected"),
              QStringLiteral("revision 25: answered disconnected"));
        waitFor([&] { return r.control.isReady(); });
        check(!r.control.hasRetries(), QStringLiteral("revision 25: not sent again"));
        const int after = windows(direct, sid);
        check(after == before + 1, QStringLiteral("revision 25: the window was made, once (%1, then %2)").arg(before).arg(after));
    }
    // ---- Reconnected to another server instance (as after a restart,
    // which forgets keys): not sent again. With a second server only.
    if (keys && argc > 2) {
        Reconnecting r(proxy.url());
        r.attach(sid);
        const int before = windows(direct, sid);
        proxy.arm(QStringLiteral("window_id"));
        proxy.retarget(QUrl(QString::fromLocal8Bit(argv[2])));   // new connections only
        bool answered = false;
        QJsonObject error;
        r.control.requestOnce(QStringLiteral("session.new_window"),
                              {{QStringLiteral("session_id"), sid}, {QStringLiteral("layout"), shell()}},
                              [&](const QJsonObject &, const QJsonObject &e) { answered = true, error = e; });
        waitFor([&] { return answered; });
        check(answered && error.value(QLatin1String("code")).toString() == QLatin1String("disconnected"),
              QStringLiteral("another instance: answered disconnected, not sent again (%1 %2)")
                  .arg(answered)
                  .arg(QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact))));
        check(windows(direct, sid) == before + 1, QStringLiteral("another instance: the window was made once"));
        proxy.retarget(server);
    }
    call(direct, QStringLiteral("session.destroy"), {{QStringLiteral("session_id"), sid}});

    // ---- A Rex tab whose session.create is cut.
    if (keys) {
        const QStringList before = sessions(direct);
        Settings settings;
        const TerminalProfile profile;
        auto *session = new RexSession(proxy.url(), QString(), settings, profile);
        QList<QPointer<TerminalTab>> tabs;
        QString ended;
        bool hasEnded = false;
        QObject::connect(session, &RexSession::tabAdded, [&](TerminalTab *tab, const QString &) {
            tabs << tab;
            tab->resize(900, 520);
            tab->show();
        });
        QObject::connect(session, &RexSession::tabRemoved, [&](TerminalTab *tab) {
            tabs.removeAll(tab);
            tab->hide();
            tab->deleteLater();
        });
        QObject::connect(session, &RexSession::ended, [&](const QString &reason) {
            ended = reason;
            hasEnded = true;
        });
        proxy.arm(QStringLiteral("initial_windows"));
        session->start();
        check(waitFor([&] { return (session->isAttached() && tabs.size() == 1) || hasEnded; }, 20000) &&
                  session->isAttached(),
              QStringLiteral("a tab whose session.create was cut: attached, one tab ") + ended);
        check(!proxy.armed(), QStringLiteral("its create was cut"));
        QStringList after = sessions(direct);
        for (const QString &s : before)
            after.removeAll(s);
        check(after.size() == 1, QStringLiteral("one new session (%1)").arg(after.join(QLatin1String(", "))));
        for (const QString &s : after)
            call(direct, QStringLiteral("session.destroy"), {{QStringLiteral("session_id"), s}});
        waitFor([&] { return hasEnded; }, 5000);
        delete session;
    }

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
