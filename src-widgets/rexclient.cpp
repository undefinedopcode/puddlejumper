/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexclient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
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

QWebSocket *newSocket(QObject *parent)
{
    // An empty origin: no Origin header.
    return new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, parent);
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

QUrl RexControl::socketUrl(const QString &path) const
{
    QUrl url = m_server;
    url.setScheme(m_server.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

void RexControl::open(const QUrl &server)
{
    close();
    m_server = server;
    m_error.clear();
    m_socket = newSocket(this);
    m_socket->setMaxAllowedIncomingMessageSize(16 << 20);
    connect(m_socket, &QWebSocket::connected, this, [this]() {
        QJsonObject client{{QStringLiteral("kind"), QStringLiteral("app")},
                           {QStringLiteral("name"), QStringLiteral("PuddleJumper")},
                           {QStringLiteral("version"), QCoreApplication::applicationVersion()},
                           {QStringLiteral("platform"), QSysInfo::productType()}};
        QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                          {QStringLiteral("api_revision"), kApiRevision},
                          {QStringLiteral("api_min_revision"), kApiMinRevision},
                          {QStringLiteral("client"), client}};
        if (!m_reconnectToken.isEmpty())
            hello.insert(QStringLiteral("reconnect_token"), m_reconnectToken);
        send(hello);
        m_helloTimer->start();
    });
    connect(m_socket, &QWebSocket::textMessageReceived, this, &RexControl::onText);
    // A connection that never opens reports an error, one that closes a
    // disconnect; fail() takes the first.
    auto gone = [this]() {
        QString reason = m_socket ? m_socket->closeReason() : QString();
        if (reason.isEmpty() && m_socket && m_socket->error() != QAbstractSocket::UnknownSocketError)
            reason = m_socket->errorString();
        fail(reason);
    };
    connect(m_socket, &QWebSocket::disconnected, this, gone);
    connect(m_socket, &QWebSocket::errorOccurred, this, gone);

    // The server gives up on a hello after 5 s; this gives up on a welcome
    // after 8 (as rexia).
    m_helloTimer = new QTimer(this);
    m_helloTimer->setSingleShot(true);
    m_helloTimer->setInterval(8000);
    connect(m_helloTimer, &QTimer::timeout, this, [this]() {
        if (!m_ready)
            fail(QStringLiteral("the server did not answer"));
    });

    QWebSocketHandshakeOptions options;
    options.setSubprotocols({kControlProtocol});
    m_socket->open(QNetworkRequest(socketUrl(QStringLiteral("/api/control"))), options);
}

void RexControl::close()
{
    if (!m_socket)
        return;
    QWebSocket *s = m_socket;
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
    close();
    emit closed(reason);
}

void RexControl::send(const QJsonObject &message)
{
    if (m_socket)
        m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void RexControl::request(const QString &method, const QJsonObject &payload, Reply reply)
{
    if (!m_socket) {
        if (reply)
            reply({}, errorObject(QStringLiteral("disconnected"), QStringLiteral("not connected")));
        return;
    }
    if (!m_ready) {
        m_waiting.append({method, payload, std::move(reply)});
        return;
    }
    const QString id = QString::number(++m_nextId);
    if (reply)
        m_replies.insert(id, std::move(reply));
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("request")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("method"), method},
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
            m_ready = true;
            m_helloTimer->stop();
            emit ready();
            for (Pending &p : std::exchange(m_waiting, {}))
                request(p.method, p.payload, std::move(p.reply));
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
        QWebSocket *s = m_socket;
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
    m_socket = newSocket(this);
    m_socket->setMaxAllowedIncomingMessageSize(64 << 20);
    connect(m_socket, &QWebSocket::connected, this, [this, hello]() {
        m_socket->sendBinaryMessage(hello);
    });
    connect(m_socket, &QWebSocket::binaryMessageReceived, this, &RexTerminalConnection::onBinary);
    connect(m_socket, &QWebSocket::disconnected, this, &RexTerminalConnection::onDisconnected);
    connect(m_socket, &QWebSocket::errorOccurred, this, &RexTerminalConnection::onDisconnected);
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({kTerminalProtocol});
    m_socket->open(QNetworkRequest(m_control->socketUrl(QStringLiteral("/api/data"))), options);
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
    // Once per socket, whichever of error and disconnect comes first.
    QWebSocket *gone = qobject_cast<QWebSocket *>(sender());
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
