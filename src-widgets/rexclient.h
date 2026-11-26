/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "ghosttysnapshot.h"
#include "rexterminalstream.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>

class QTimer;
class RexSocket;

/*
 * A Rex server's control connection: WebSocket /api/control, subprotocol
 * rex.control.v1, JSON text messages.
 *
 *   -> {"type":"hello", "api_revision", "api_min_revision", "client":{...}}
 *   <- {"type":"welcome", "protocol", "client":{"id","reconnect_token"}, ...}
 *   -> {"type":"request", "id", "method", "payload"}
 *   <- {"type":"response", "id", "seq", "payload"} or {..., "error":{code,message,status}}
 *   <- {"type":"event", "seq", ...}
 *
 * Rex 0.1.0 asks for no API revision in the hello; later ones do, and the
 * range sent is rexia's (8-18). See the Rex client spec and the tuxosaurus
 * cross-check for where each piece comes from.
 *
 * No Origin header is sent: a Rex server refuses Origins it doesn't allow.
 */
class RexControl : public QObject
{
    Q_OBJECT

public:
    // `payload` on success; otherwise `error` has code, message and status
    // (a lost connection is code "disconnected", status 0).
    using Reply = std::function<void(const QJsonObject &payload, const QJsonObject &error)>;

    explicit RexControl(QObject *parent = nullptr);
    ~RexControl() override;

    // The server's address: http(s)://host:port, or unix:///path/to/socket
    // (rexaddress); the WebSockets follow.
    void open(const QUrl &server);
    void close();

    bool isReady() const { return m_ready; }
    QUrl server() const { return m_server; }
    QString clientId() const { return m_clientId; }
    QString lastError() const { return m_error; }

    // Requests sent before ready() wait for it.
    void request(const QString &method, const QJsonObject &payload = {}, Reply reply = {});

    // A WebSocket to `path` on the server (TCP or its Unix socket).
    RexSocket *newSocket(const QString &path, const QString &subprotocol, qint64 maxMessage, QObject *parent) const;

    static constexpr int kApiRevision = 21;
    static constexpr int kApiMinRevision = 8;

signals:
    void ready();
    // Every event message, whole.
    void event(const QJsonObject &event);
    // The connection is gone; pending requests have been answered with an
    // error. `reason` may be empty.
    void closed(const QString &reason);

private:
    void onText(const QString &text);
    void send(const QJsonObject &message);
    void fail(const QString &reason);

    struct Pending
    {
        QString method;
        QJsonObject payload;
        Reply reply;
    };

    RexSocket *m_socket = nullptr;
    QTimer *m_helloTimer = nullptr;
    QUrl m_server;
    bool m_ready = false;
    QString m_clientId;
    QString m_reconnectToken;
    QString m_error;
    quint64 m_nextId = 0;
    QHash<QString, Reply> m_replies;
    QList<Pending> m_waiting;  // before ready
};

/*
 * One terminal block's output: a ticket from data.connect, WebSocket
 * /api/data with subprotocol rex.data.terminal.v1, then binary messages.
 *
 *   -> 01 ticket[16]                       (hello)
 *   -> 02 token[16] offset:u64le           (resume)
 *   <- 81 token[16] 0 0 0 grace:u32le nonce:u64le   (welcome, 32 bytes)
 *   <- 82 reason                            (reject; 02: the terminal is mid
 *                                            escape sequence, try again)
 *
 * then the stream RexTerminalStream takes apart. A connection that drops is
 * resumed from the bytes already passed on, within the grace the welcome
 * gave; if that is refused, a new ticket and snapshot are taken
 * (snapshot() again: the receiver starts over from it).
 */
class RexTerminalConnection : public QObject
{
    Q_OBJECT

public:
    RexTerminalConnection(RexControl *control, const QString &sessionId, const QString &blockId,
                          QObject *parent = nullptr);
    ~RexTerminalConnection() override;

    void open();
    void close();
    // A fresh stream and snapshot, once the current events are delivered.
    // For when the terminal's width changes: Ghostty rewraps lines and
    // qtermwidget doesn't, so the server's snapshot is the only way to show
    // what it has.
    void resync();

    QString sessionId() const { return m_sessionId; }
    QString blockId() const { return m_blockId; }
    bool isOpen() const { return m_state == State::Streaming; }
    // Stream bytes passed on so far (snapshot and markers included).
    quint64 consumed() const { return m_stream ? m_stream->consumed() : 0; }

    // For tests: drop the socket as a network failure would.
    void dropForTest();

signals:
    void snapshot(const GhosttySnapshot::Snapshot &snapshot);
    void output(const QByteArray &bytes);
    void marker(const QJsonObject &marker);
    // Lost; resuming.
    void interrupted();
    void resumed();
    // Given up (closed by the server for good, or no way back in). A block
    // whose program exits is closed, and its stream with it; the control
    // connection's child_exited event says so first.
    void failed(const QString &reason);

private:
    enum class State { Idle, Ticket, Handshake, Streaming, Resuming, Closed };

    void requestTicket();
    void connectSocket(const QByteArray &hello);
    void onBinary(const QByteArray &data);
    void onDisconnected();
    void retryFresh(int delayMs);

    QPointer<RexControl> m_control;
    QString m_sessionId, m_blockId;
    RexSocket *m_socket = nullptr;
    State m_state = State::Idle;
    bool m_resuming = false;  // the socket's hello was a resume
    std::unique_ptr<RexTerminalStream> m_stream;
    QByteArray m_token;
    quint64 m_nonce = 0;
    quint32 m_graceMs = 0;
    qint64 m_lostAt = 0;  // ms since epoch
    int m_freshTries = 0;
};
