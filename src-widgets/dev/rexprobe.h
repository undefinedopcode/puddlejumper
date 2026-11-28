/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * rexprobe.h: what the dev probes (probe-osc6522, probe-kitty) share. One
 * session on a Rex-compatible server whose one block runs a command, the
 * block's terminal stream as RexTerminalConnection passes it on (every
 * output byte, each snapshot, and qtermwidget's emulator fed from them), and
 * input written as RexSession::sendInput writes it.
 */

#pragma once

#include "ghosttyreplay.h"
#include "qtermprobe.h"
#include "rexclient.h"
#include "rexsocket.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <functional>
#include <memory>
#include <optional>

namespace rexprobe {

inline bool waitFor(const std::function<bool()> &done, int ms)
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

inline Call call(RexControl &c, const QString &method, const QJsonObject &payload, int ms = 15000)
{
    auto result = std::make_shared<Call>();
    c.request(method, payload, [result](const QJsonObject &p, const QJsonObject &e) {
        result->payload = p;
        result->error = e;
        result->done = true;
    });
    if (!waitFor([&] { return result->done; }, ms))
        result->error = QJsonObject{{QStringLiteral("code"), QStringLiteral("timeout")}};
    return *result;
}

inline QString errorText(const QJsonObject &e)
{
    return QString::fromUtf8(QJsonDocument(e).toJson(QJsonDocument::Compact));
}

// A session whose one block runs `command` (with no shell), streamed.
class Block
{
public:
    // Connects, makes the session at 80x24 and opens the stream. On failure
    // returns false with error() set; whatever was made is destroyed.
    bool open(const QString &server, const QStringList &command, const QString &label)
    {
        control.open(rexaddress::parse(server));
        if (!waitFor([&] { return control.isReady() || !control.lastError().isEmpty(); }, 10000) || !control.isReady())
            return fail(QStringLiteral("control connection: ") + control.lastError());
        const QJsonObject block{{QStringLiteral("flavor"), QStringLiteral("com.superlogical.terminal.shell")},
                                {QStringLiteral("options"),
                                 QJsonObject{{QStringLiteral("command"), QJsonArray::fromStringList(command)},
                                             {QStringLiteral("shell"), QStringLiteral("none")},
                                             {QStringLiteral("login"), false}}}};
        const Call created = call(control, QStringLiteral("session.create"),
                                  QJsonObject{{QStringLiteral("label"), label},
                                              {QStringLiteral("initial_windows"),
                                               QJsonArray{QJsonObject{{QStringLiteral("layout"),
                                                                       QJsonObject{{QStringLiteral("block"), block}}}}}}});
        m_session = created.payload.value(QLatin1String("session_id")).toString();
        if (m_session.isEmpty())
            return fail(QStringLiteral("session.create: ") + errorText(created.error));
        const Call attached = call(control, QStringLiteral("session.attach"), QJsonObject{{QStringLiteral("session_id"), m_session}});
        const QString bid = attached.payload.value(QLatin1String("view")).toObject()
                                .value(QLatin1String("windows")).toArray().first().toObject()
                                .value(QLatin1String("focused_block_id")).toString();
        if (bid.isEmpty())
            return fail(QStringLiteral("session.attach: no block ") + errorText(attached.error));
        target = QJsonObject{{QStringLiteral("session_id"), m_session}, {QStringLiteral("block_id"), bid}};
        blockCall(QStringLiteral("com.superlogical.terminal.resize"),
                  QJsonObject{{QStringLiteral("columns"), 80}, {QStringLiteral("rows"), 24}});

        m_terminal = std::make_unique<RexTerminalConnection>(&control, m_session, bid);
        QObject::connect(m_terminal.get(), &RexTerminalConnection::snapshot, [this](const GhosttySnapshot::Snapshot &s) {
            ++snapshots;
            snapshot = s;
            emu = std::make_unique<QtermProbe>(s.terminal.columns, s.terminal.rows);
            emu->feed(GhosttySnapshot::replay(s));
        });
        QObject::connect(m_terminal.get(), &RexTerminalConnection::output, [this](const QByteArray &b) {
            ++outputEvents;
            stream += b;
            if (emu)
                emu->feed(b);
        });
        QObject::connect(m_terminal.get(), &RexTerminalConnection::failed, [this](const QString &why) { failure = why; });
        m_terminal->open();
        if (!waitFor([&] { return snapshots == 1; }, 10000))
            return fail(QStringLiteral("no snapshot: ") + failure);
        return true;
    }

    // A new stream, and with it a new snapshot.
    bool reopen()
    {
        m_terminal->close();
        const int before = snapshots;
        m_terminal->open();
        return waitFor([&] { return snapshots > before; }, 10000);
    }

    void close()
    {
        if (m_terminal)
            m_terminal->close();
        if (!m_session.isEmpty())
            call(control, QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), m_session}});
        m_session.clear();
        control.close();
    }

    Call blockCall(const QString &method, const QJsonObject &args)
    {
        QJsonObject p = target;
        p.insert(QStringLiteral("args"), args);
        return call(control, method, p);
    }

    // As RexSession::sendInput: 32 KiB a request.
    bool write(const QByteArray &bytes)
    {
        for (qsizetype i = 0; i < bytes.size(); i += 32 * 1024) {
            const Call r = blockCall(QStringLiteral("com.superlogical.terminal.write"),
                                     QJsonObject{{QStringLiteral("data"), QString::fromUtf8(bytes.mid(i, 32 * 1024))}});
            if (!r.error.isEmpty()) {
                writeError = errorText(r.error);
                return false;
            }
        }
        return true;
    }

    // The line of output starting with `prefix` after `from`, once it has
    // arrived whole (CR LF), without the CR LF.
    std::optional<QByteArray> lineAfter(qsizetype from, const QByteArray &prefix, int ms)
    {
        qsizetype at = -1, end = -1;
        if (!waitFor([&] {
                at = stream.indexOf(prefix, from);
                end = at < 0 ? -1 : stream.indexOf("\r\n", at);
                return end >= 0;
            }, ms))
            return std::nullopt;
        return stream.mid(at, end - at);
    }

    // The server's text of the terminal (com.superlogical.terminal.format).
    QString serverScreen()
    {
        QJsonObject args = target;
        args.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("trim"), true}});
        return call(control, QStringLiteral("com.superlogical.terminal.format"), args)
            .payload.value(QLatin1String("content")).toString();
    }
    QString clientScreen() const
    {
        return emu ? QtermProbeLines::screenLines(emu->screen(0)).join(QLatin1Char('\n')) : QString();
    }

    QString error() const { return m_error; }

    RexControl control;
    QJsonObject target;
    QByteArray stream;  // every output byte since the stream opened, snapshots aside
    int outputEvents = 0, snapshots = 0;
    GhosttySnapshot::Snapshot snapshot;  // the latest
    std::unique_ptr<QtermProbe> emu;
    QString failure, writeError;

private:
    bool fail(const QString &why)
    {
        m_error = why;
        close();
        return false;
    }

    QString m_session, m_error;
    std::unique_ptr<RexTerminalConnection> m_terminal;
};

} // namespace rexprobe
