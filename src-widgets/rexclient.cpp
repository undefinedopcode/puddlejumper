/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexclient.h"
#include "rexsocket.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

namespace {

const QString kControlProtocol = QStringLiteral("rex.control.v1");
const QString kTerminalProtocol = QStringLiteral("rex.data.terminal.v1");

QJsonObject errorObject(const QString &code, const QString &message, int status = 0)
{
    return QJsonObject{{QStringLiteral("code"), code},
                       {QStringLiteral("message"), message},
                       {QStringLiteral("status"), status}};
}

} // namespace

// ---- Control. ---------------------------------------------------------------

RexControl::RexControl(QObject *parent)
    : QObject(parent)
{
}

RexControl::~RexControl()
{
    if (m_socket)
        m_socket->abort();
}

RexSocket *RexControl::newSocket(const QString &path, const QString &subprotocol, qint64 maxMessage,
                                 QObject *parent) const
{
    auto *socket = new RexSocket(parent);
    socket->setMaxAllowedIncomingMessageSize(maxMessage);
    socket->open(m_server, path, subprotocol);
    return socket;
}

void RexControl::open(const QUrl &server)
{
    // Calls waiting to be sent again survive a reconnect to the same server.
    QList<Pending> retry;
    if (server == m_server)
        retry = std::exchange(m_retry, {});
    close();
    m_retry = std::move(retry);
    m_server = server;
    m_error.clear();
    // The timer before the socket: a Unix socket can connect at once.
    m_helloTimer = new QTimer(this);
    m_helloTimer->setSingleShot(true);
    m_helloTimer->setInterval(8000);
    connect(m_helloTimer, &QTimer::timeout, this, [this]() {
        if (!m_ready)
            fail(QStringLiteral("the server did not answer"));
    });
    m_socket = newSocket(QStringLiteral("/api/control"), kControlProtocol, 16 << 20, this);
    connect(m_socket, &RexSocket::connected, this, [this]() {
        QJsonObject client{{QStringLiteral("kind"), QStringLiteral("app")},
                           {QStringLiteral("name"), QStringLiteral("PuddleJumper")},
                           {QStringLiteral("version"), QCoreApplication::applicationVersion()},
                           {QStringLiteral("platform"), QSysInfo::productType()}};
        QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                          {QStringLiteral("api_revision"), m_asked},
                          {QStringLiteral("api_min_revision"), kApiMinRevision},
                          {QStringLiteral("client"), client}};
        if (!m_reconnectToken.isEmpty())
            hello.insert(QStringLiteral("reconnect_token"), m_reconnectToken);
        send(hello);
        m_helloTimer->start();
    });
    connect(m_socket, &RexSocket::textMessageReceived, this, &RexControl::onText);
    connect(m_socket, &RexSocket::disconnected, this, [this]() {
        fail(m_socket ? m_socket->failureReason() : QString());
    });
    // (The server gives up on a hello after 5 s; the timer above on a
    // welcome after 8, as rexia.)
}

void RexControl::close()
{
    m_closing = true;
    dropSocket();
    m_closing = false;
    for (const Pending &p : std::exchange(m_retry, {}))
        if (p.reply)
            p.reply({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("closed")));
}

void RexControl::dropSocket()
{
    if (!m_socket)
        return;
    RexSocket *s = m_socket;
    m_socket = nullptr;
    s->disconnect(this);
    s->close();
    s->deleteLater();
    if (m_helloTimer) {
        m_helloTimer->deleteLater();
        m_helloTimer = nullptr;
    }
    const bool wasReady = m_ready;
    m_ready = false;
    m_keyed.clear();
    const auto replies = std::exchange(m_replies, {});
    const auto waiting = std::exchange(m_waiting, {});
    for (const Reply &r : replies)
        if (r)
            r({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("closed")));
    for (const Pending &p : waiting)
        if (p.reply)
            p.reply({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("closed")));
    Q_UNUSED(wasReady);
}

void RexControl::fail(const QString &reason)
{
    if (!m_socket)
        return;
    m_error = reason;
    // A call sent with a key and not answered may have been done or not:
    // sent again with the key after the next welcome, it is done once.
    for (auto it = m_keyed.begin(); it != m_keyed.end(); ++it) {
        m_replies.remove(it.key());
        m_retry.append(it.value());
    }
    m_keyed.clear();
    dropSocket();
    emit closed(reason);
}

void RexControl::send(const QJsonObject &message)
{
    if (m_socket)
        m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void RexControl::request(const QString &method, const QJsonObject &payload, Reply reply)
{
    dispatch({method, payload, std::move(reply)});
}

void RexControl::requestOnce(const QString &method, const QJsonObject &payload, Reply reply)
{
    Pending p{method, payload, std::move(reply)};
    p.once = true;
    dispatch(std::move(p));
}

void RexControl::dispatch(Pending p)
{
    if (!m_socket) {
        if (p.reply)
            p.reply({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("not connected")));
        return;
    }
    if (!m_ready) {
        m_waiting.append(std::move(p));
        return;
    }
    // Which server instance a key goes to (a restart forgets keys): the
    // welcome says, for atlantis; Rex's server.status does, asked first.
    if (p.once && m_revision >= kIdempotencyRevision && m_instance.isEmpty() && !std::exchange(m_askedInstance, true))
        askInstance([this](const QString &instance) { m_instance = instance; });
    const QString id = QString::number(++m_nextId);
    QJsonObject payload = p.payload;
    if (p.once && m_revision >= kIdempotencyRevision) {
        if (p.key.isEmpty()) {
            p.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
            p.since = QDateTime::currentMSecsSinceEpoch();
        }
        payload.insert(QStringLiteral("idempotency_key"), p.key);
        m_keyed.insert(id, p);   // until answered, to send again
        p.reply = [this, id, reply = std::move(p.reply)](const QJsonObject &result, const QJsonObject &error) {
            m_keyed.remove(id);
            if (reply)
                reply(result, error);
        };
    }
    if (p.reply)
        m_replies.insert(id, std::move(p.reply));
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("request")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("method"), p.method},
                     {QStringLiteral("payload"), payload}});
}

void RexControl::onText(const QString &text)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;  // as rexia: what can't be read is dropped
    const QJsonObject m = doc.object();
    const QString type = m.value(QLatin1String("type")).toString();

    if (!m_ready) {
        if (type == QLatin1String("welcome")) {
            const QJsonObject client = m.value(QLatin1String("client")).toObject();
            m_clientId = client.value(QLatin1String("id")).toString();
            m_reconnectToken = client.value(QLatin1String("reconnect_token")).toString();
            m_revision = m.value(QLatin1String("api_revision")).toInt();
            const QString instance =
                m.value(QLatin1String("server")).toObject().value(QLatin1String("instance_id")).toString();
            const QString sentTo = std::exchange(m_instance, instance);
            m_askedInstance = false;
            m_ready = true;
            m_helloTimer->stop();
            // Before ready(): its handlers may close or reopen.
            auto retry = std::exchange(m_retry, {});
            emit ready();
            for (Pending &p : std::exchange(m_waiting, {}))
                dispatch(std::move(p));
            if (!m_ready && m_socket)
                m_retry = std::move(retry) + m_retry;   // reopened: for the next welcome
            else if (!m_ready)
                resend(std::move(retry), false);        // closed
            else if (retry.isEmpty())
                return;
            else if (m_revision < kIdempotencyRevision || sentTo.isEmpty())
                resend(std::move(retry), false);
            else if (!instance.isEmpty())
                resend(std::move(retry), instance == sentTo);
            else {
                m_askedInstance = true;
                askInstance([this, sentTo, retry = std::move(retry)](const QString &now) mutable {
                    if (!m_ready && !m_closing) {
                        m_retry = std::move(retry) + m_retry;   // dropped again first: next time
                        m_instance = sentTo;
                        return;
                    }
                    m_instance = now;
                    resend(std::move(retry), !now.isEmpty() && now == sentTo);
                });
            }
        } else if (type == QLatin1String("error") || type == QLatin1String("response")) {
            const QJsonObject e = m.value(QLatin1String("error")).toObject();
            QString why = e.value(QLatin1String("message")).toString();
            if (why.isEmpty())
                why = m.value(QLatin1String("message")).toString();
            fail(why.isEmpty() ? QStringLiteral("refused") : why);
        }
        return;  // anything else before the welcome is dropped
    }

    if (type == QLatin1String("response")) {
        const Reply reply = m_replies.take(m.value(QLatin1String("id")).toString());
        if (!reply)
            return;
        const QJsonValue e = m.value(QLatin1String("error"));
        if (e.isObject())
            reply({}, e.toObject());
        else
            reply(m.value(QLatin1String("payload")).toObject(), {});
    } else if (type == QLatin1String("event")) {
        emit event(m);
    }
}

void RexControl::askInstance(std::function<void(const QString &)> done)
{
    request(QStringLiteral("server.status"), {}, [done](const QJsonObject &payload, const QJsonObject &) {
        done(payload.value(QLatin1String("instance_id")).toString());
    });
}

// Calls cut off by a dropped connection: sent again with their keys if
// `same` (the instance they went to) and not too old, else answered
// "disconnected".
void RexControl::resend(QList<Pending> calls, bool same)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Pending &p : calls) {
        if (same && m_ready && now - p.since < kRetryForMs)
            dispatch(std::move(p));
        else if (p.reply)
            p.reply({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("closed")));
    }
}

// ---- Terminal data. -----------------------------------------------------------

RexTerminalConnection::RexTerminalConnection(RexControl *control, const QString &sessionId,
                                             const QString &blockId, QObject *parent)
    : QObject(parent)
    , m_control(control)
    , m_sessionId(sessionId)
    , m_blockId(blockId)
{
}

RexTerminalConnection::~RexTerminalConnection()
{
    if (m_socket)
        m_socket->abort();
}

void RexTerminalConnection::open()
{
    m_freshTries = 0;
    requestTicket();
}

void RexTerminalConnection::close()
{
    m_state = State::Closed;
    if (m_socket) {
        RexSocket *s = m_socket;
        m_socket = nullptr;
        s->disconnect(this);
        s->close();
        s->deleteLater();
    }
}

void RexTerminalConnection::resync()
{
    QMetaObject::invokeMethod(this, [this]() {
        if (m_state == State::Closed || m_state == State::Idle)
            return;
        close();
        open();
    }, Qt::QueuedConnection);
}

void RexTerminalConnection::dropForTest()
{
    if (m_socket)
        m_socket->abort();
}

void RexTerminalConnection::requestTicket()
{
    if (!m_control) {
        m_state = State::Closed;
        emit failed(QStringLiteral("no control connection"));
        return;
    }
    m_state = State::Ticket;
    m_stream.reset();
    QPointer<RexTerminalConnection> self(this);
    m_control->request(
        QStringLiteral("data.connect"),
        QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                    {QStringLiteral("block_id"), m_blockId},
                    {QStringLiteral("data_ref_id"), QStringLiteral("terminal")},
                    {QStringLiteral("subprotocol"), kTerminalProtocol}},
        [self](const QJsonObject &payload, const QJsonObject &error) {
            if (!self || self->m_state != State::Ticket)
                return;
            const QByteArray ticket = QByteArray::fromBase64(
                payload.value(QLatin1String("ticket")).toString().toLatin1(),
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            if (!error.isEmpty() || ticket.size() != 16) {
                self->m_state = State::Closed;
                emit self->failed(error.isEmpty() ? QStringLiteral("no ticket")
                                                  : error.value(QLatin1String("message")).toString());
                return;
            }
            self->m_resuming = false;
            self->connectSocket('\x01' + ticket);
        });
}

void RexTerminalConnection::connectSocket(const QByteArray &hello)
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_state = m_resuming ? State::Resuming : State::Handshake;
    m_socket = m_control->newSocket(QStringLiteral("/api/data"), kTerminalProtocol, 64 << 20, this);
    connect(m_socket, &RexSocket::connected, this, [this, hello]() {
        m_socket->sendBinaryMessage(hello);
    });
    connect(m_socket, &RexSocket::binaryMessageReceived, this, &RexTerminalConnection::onBinary);
    connect(m_socket, &RexSocket::disconnected, this, &RexTerminalConnection::onDisconnected);
}

void RexTerminalConnection::onBinary(const QByteArray &data)
{
    if (m_state == State::Handshake || m_state == State::Resuming) {
        if (!data.isEmpty() && quint8(data[0]) == 0x82) {
            const int reason = data.size() > 1 ? quint8(data[1]) : 0;
            if (m_state == State::Resuming) {
                // Too late, or gone from the buffer: start over.
                retryFresh(0);
            } else if (reason == 2) {
                // The terminal is in the middle of an escape sequence; the
                // server recovers within 250 ms.
                retryFresh(300);
            } else {
                m_state = State::Closed;
                emit failed(QStringLiteral("refused (reason %1)").arg(reason));
            }
            return;
        }
        quint64 nonce = 0;
        quint32 grace = 0;
        QByteArray token;
        if (!RexTerminalStream::parseWelcome(data, &nonce, &grace, &token)) {
            m_state = State::Closed;
            emit failed(QStringLiteral("not a welcome"));
            return;
        }
        m_graceMs = grace;
        m_token = token;
        if (m_state == State::Resuming) {
            // The same stream, carrying on: a new parser from where the old
            // one had passed bytes on.
            const quint64 offset = m_stream ? m_stream->consumed() : 0;
            m_stream = std::make_unique<RexTerminalStream>(nonce, true, offset);
            m_nonce = nonce;
            m_state = State::Streaming;
            emit resumed();
        } else {
            m_nonce = nonce;
            m_stream = std::make_unique<RexTerminalStream>(nonce);
            m_state = State::Streaming;
        }
        return;
    }
    if (m_state != State::Streaming || !m_stream)
        return;

    for (auto &e : m_stream->feed(data)) {
        switch (e.kind) {
        case RexTerminalStream::Event::Snapshot:
            m_freshTries = 0;
            emit snapshot(*e.snapshot);
            break;
        case RexTerminalStream::Event::Output:
            emit output(e.bytes);
            break;
        case RexTerminalStream::Event::Marker:
            emit marker(e.marker);
            break;
        }
        if (m_state != State::Streaming)
            return;  // a receiver closed us
    }
    if (m_stream->failed()) {
        m_state = State::Closed;
        emit failed(m_stream->error());
        if (m_socket)
            m_socket->abort();
    }
}

void RexTerminalConnection::onDisconnected()
{
    // Once per socket.
    RexSocket *gone = qobject_cast<RexSocket *>(sender());
    if (!gone || gone != m_socket)
        return;
    m_socket->disconnect(this);
    m_socket->deleteLater();
    m_socket = nullptr;
    if (m_state == State::Closed || m_state == State::Idle)
        return;
    const bool canResume = m_stream && m_stream->haveSnapshot() && m_token.size() == 16 && m_graceMs > 0;
    if (m_state == State::Streaming && canResume) {
        m_lostAt = QDateTime::currentMSecsSinceEpoch();
        emit interrupted();
        QByteArray hello(1, '\x02');
        hello += m_token;
        char offset[8];
        qToLittleEndian<quint64>(m_stream->consumed(), offset);
        hello.append(offset, 8);
        m_resuming = true;
        QTimer::singleShot(200, this, [this, hello]() {
            if (m_state == State::Streaming || m_state == State::Resuming)
                connectSocket(hello);
        });
        m_state = State::Resuming;
        return;
    }
    if (m_state == State::Resuming
        && QDateTime::currentMSecsSinceEpoch() - m_lostAt < qint64(m_graceMs)) {
        // Not back yet: try again until the grace runs out.
        QByteArray hello(1, '\x02');
        hello += m_token;
        char offset[8];
        qToLittleEndian<quint64>(m_stream ? m_stream->consumed() : 0, offset);
        hello.append(offset, 8);
        QTimer::singleShot(1000, this, [this, hello]() {
            if (m_state == State::Resuming)
                connectSocket(hello);
        });
        return;
    }
    retryFresh(1000);
}

void RexTerminalConnection::retryFresh(int delayMs)
{
    if (++m_freshTries > 5) {
        m_state = State::Closed;
        emit failed(QStringLiteral("the terminal stream keeps failing"));
        return;
    }
    m_state = State::Ticket;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    QTimer::singleShot(delayMs, this, [this]() {
        if (m_state == State::Ticket)
            requestTicket();
    });
}
