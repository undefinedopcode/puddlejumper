/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexsocket.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QtEndian>

// ---- Addresses. -------------------------------------------------------------

namespace rexaddress {

QString defaultSocket()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return {};
    return QDir(runtime).filePath(QStringLiteral("rex/server.sock"));
}

QUrl parse(const QString &typed)
{
    QString t = typed.trimmed();
    if (t.isEmpty())
        return {};
    if (t.startsWith(QLatin1Char('/')))
        t.prepend(QLatin1String("unix://"));
    if (t == QLatin1String("unix:") || t == QLatin1String("unix://")) {
        const QString path = defaultSocket();
        if (path.isEmpty())
            return {};
        t = QLatin1String("unix://") + path;
    }
    if (!t.contains(QLatin1String("://")))
        t.prepend(QLatin1String("http://"));
    const QUrl url(t);
    return isValid(url) ? url : QUrl();
}

bool isUnix(const QUrl &server)
{
    return server.scheme() == QLatin1String("unix");
}

QString socketPath(const QUrl &server)
{
    return isUnix(server) ? server.path() : QString();
}

bool isValid(const QUrl &server)
{
    if (!server.isValid())
        return false;
    if (isUnix(server))
        return server.path().startsWith(QLatin1Char('/'));
    const QString scheme = server.scheme();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https")) && !server.host().isEmpty();
}

QString displayName(const QUrl &server)
{
    if (!isUnix(server))
        return server.authority();
    QString path = socketPath(server);
    const QString home = QDir::homePath();
    if (path.startsWith(home + QLatin1Char('/')))
        path = QLatin1Char('~') + path.mid(home.size());
    return path;
}

} // namespace rexaddress

// ---- The socket. ------------------------------------------------------------

namespace {

constexpr quint8 kContinuation = 0x0, kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA;

QByteArray acceptFor(const QByteArray &key)
{
    return QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1)
        .toBase64();
}

// A header's value from an HTTP response head ("" when absent).
QByteArray header(const QByteArray &head, const QByteArray &name)
{
    for (const QByteArray &line : head.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
            return line.mid(colon + 1).trimmed();
    }
    return {};
}

} // namespace

RexSocket::RexSocket(QObject *parent)
    : QObject(parent)
{
}

RexSocket::~RexSocket()
{
    if (m_local)
        m_local->disconnect(this);
    if (m_web)
        m_web->disconnect(this);
}

void RexSocket::open(const QUrl &server, const QString &path, const QString &subprotocol)
{
    m_path = path;
    m_subprotocol = subprotocol;
    if (!rexaddress::isUnix(server)) {
        // An empty origin: no Origin header (a Rex server refuses Origins
        // it doesn't allow).
        m_web = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        m_web->setMaxAllowedIncomingMessageSize(m_maxMessage);
        connect(m_web, &QWebSocket::connected, this, &RexSocket::connected);
        connect(m_web, &QWebSocket::textMessageReceived, this, &RexSocket::textMessageReceived);
        connect(m_web, &QWebSocket::binaryMessageReceived, this, &RexSocket::binaryMessageReceived);
        auto gone = [this]() {
            QString reason = m_web->closeReason();
            if (reason.isEmpty() && m_web->error() != QAbstractSocket::UnknownSocketError)
                reason = m_web->errorString();
            finish(reason);
        };
        connect(m_web, &QWebSocket::disconnected, this, gone);
        connect(m_web, &QWebSocket::errorOccurred, this, gone);
        QUrl url = server;
        url.setScheme(server.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
        url.setPath(path);
        url.setQuery(QString());
        url.setFragment(QString());
        QWebSocketHandshakeOptions options;
        options.setSubprotocols({subprotocol});
        m_web->open(QNetworkRequest(url), options);
        return;
    }
    m_local = new QLocalSocket(this);
    connect(m_local, &QLocalSocket::connected, this, &RexSocket::onLocalConnected);
    connect(m_local, &QLocalSocket::readyRead, this, &RexSocket::onLocalData);
    connect(m_local, &QLocalSocket::disconnected, this, &RexSocket::onLocalGone);
    connect(m_local, &QLocalSocket::errorOccurred, this, &RexSocket::onLocalGone);
    // From the event loop: a Unix socket can connect inside the call, before
    // the caller has connected to connected().
    const QString socket = rexaddress::socketPath(server);
    QTimer::singleShot(0, this, [this, socket]() {
        if (m_local && !m_done)
            m_local->connectToServer(socket);
    });
}

void RexSocket::sendTextMessage(const QString &text)
{
    if (m_web)
        m_web->sendTextMessage(text);
    else if (m_upgraded && !m_closing)
        sendFrame(kText, text.toUtf8());
}

void RexSocket::sendBinaryMessage(const QByteArray &data)
{
    if (m_web)
        m_web->sendBinaryMessage(data);
    else if (m_upgraded && !m_closing)
        sendFrame(kBinary, data);
}

void RexSocket::abort()
{
    if (m_web)
        m_web->abort();
    if (m_local)
        m_local->abort();
}

void RexSocket::close()
{
    if (m_web)
        m_web->close();
    if (m_local) {
        if (m_upgraded && !m_closing) {
            QByteArray normal(2, Qt::Uninitialized);
            qToBigEndian<quint16>(1000, reinterpret_cast<uchar *>(normal.data()));
            sendFrame(kClose, normal);
            m_closing = true;
        }
        m_local->disconnectFromServer();
    }
}

QString RexSocket::failureReason() const
{
    return m_reason;
}

void RexSocket::finish(const QString &reason)
{
    if (m_done)
        return;
    m_done = true;
    m_reason = reason;
    emit disconnected();
}

// ---- Over a Unix socket. ----------------------------------------------------

void RexSocket::onLocalConnected()
{
    QByteArray nonce(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(nonce.data()), 4);
    m_key = nonce.toBase64();
    QByteArray request = "GET " + m_path.toUtf8() + " HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "Sec-WebSocket-Key: " + m_key + "\r\n";
    if (!m_subprotocol.isEmpty())
        request += "Sec-WebSocket-Protocol: " + m_subprotocol.toUtf8() + "\r\n";
    request += "\r\n";
    m_local->write(request);
}

void RexSocket::onLocalData()
{
    m_buffer += m_local->readAll();
    if (!m_upgraded && !readHandshake())
        return;
    readFrames();
}

void RexSocket::onLocalGone()
{
    QString reason = m_reason;
    if (reason.isEmpty() && m_local && m_local->error() != QLocalSocket::UnknownSocketError
        && m_local->error() != QLocalSocket::PeerClosedError)
        reason = m_local->errorString();
    finish(reason);
}

bool RexSocket::readHandshake()
{
    const qsizetype end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > 16384)
            m_local->abort();
        return false;
    }
    const QByteArray head = m_buffer.left(end);
    m_buffer.remove(0, end + 4);
    const QByteArray status = head.left(head.indexOf('\r'));
    const QList<QByteArray> parts = status.split(' ');
    if (parts.size() < 2 || parts.at(1) != "101") {
        // Refused: the status line says why (a 4xx with a message, say).
        m_reason = QString::fromUtf8(status.mid(status.indexOf(' ') + 1));
        if (!m_buffer.isEmpty())
            m_reason += QLatin1String(": ") + QString::fromUtf8(m_buffer.left(200)).trimmed();
        m_local->abort();
        return false;
    }
    if (header(head, "Sec-WebSocket-Accept") != acceptFor(m_key)) {
        m_reason = tr("the server's WebSocket handshake didn't match");
        m_local->abort();
        return false;
    }
    if (!m_subprotocol.isEmpty() && header(head, "Sec-WebSocket-Protocol") != m_subprotocol.toUtf8()) {
        m_reason = tr("the server doesn't speak %1").arg(m_subprotocol);
        m_local->abort();
        return false;
    }
    m_upgraded = true;
    emit connected();
    return m_local != nullptr;  // still here (a slot may have closed it)
}

void RexSocket::readFrames()
{
    while (m_local && !m_done) {
        if (m_buffer.size() < 2)
            return;
        const auto *b = reinterpret_cast<const uchar *>(m_buffer.constData());
        const bool fin = b[0] & 0x80;
        const quint8 opcode = b[0] & 0x0F;
        const bool masked = b[1] & 0x80;
        quint64 length = b[1] & 0x7F;
        qsizetype at = 2;
        if (length == 126) {
            if (m_buffer.size() < 4)
                return;
            length = qFromBigEndian<quint16>(b + 2);
            at = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10)
                return;
            length = qFromBigEndian<quint64>(b + 2);
            at = 10;
        }
        if (length > quint64(m_maxMessage)) {
            m_reason = tr("a message from the server was too large");
            m_local->abort();
            return;
        }
        uchar mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (m_buffer.size() < at + 4)
                return;
            memcpy(mask, b + at, 4);
            at += 4;
        }
        if (quint64(m_buffer.size() - at) < length)
            return;
        QByteArray payload = m_buffer.mid(at, qsizetype(length));
        m_buffer.remove(0, at + qsizetype(length));
        if (masked)
            for (qsizetype i = 0; i < payload.size(); ++i)
                payload[i] = char(payload[i] ^ mask[i % 4]);

        switch (opcode) {
        case kPing:
            sendFrame(kPong, payload);
            break;
        case kPong:
            break;
        case kClose:
            if (payload.size() > 2 && m_reason.isEmpty())
                m_reason = QString::fromUtf8(payload.mid(2));
            if (!m_closing) {
                sendFrame(kClose, payload.left(2));
                m_closing = true;
            }
            m_local->disconnectFromServer();
            return;
        case kText:
        case kBinary:
        case kContinuation: {
            if (opcode != kContinuation) {
                m_messageOpcode = opcode;
                m_message = payload;
            } else {
                m_message += payload;
            }
            if (m_message.size() > m_maxMessage) {
                m_reason = tr("a message from the server was too large");
                m_local->abort();
                return;
            }
            if (!fin)
                break;
            const QByteArray message = std::exchange(m_message, QByteArray());
            if (m_messageOpcode == kText)
                emit textMessageReceived(QString::fromUtf8(message));
            else
                emit binaryMessageReceived(message);
            break;
        }
        default:
            m_reason = tr("the server sent a frame this client doesn't know");
            m_local->abort();
            return;
        }
    }
}

void RexSocket::sendFrame(quint8 opcode, const QByteArray &payload)
{
    if (!m_local)
        return;
    // A client's frames are masked (RFC 6455 5.3), whole (FIN), unfragmented.
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(char(0x80 | opcode));
    const qsizetype n = payload.size();
    if (n < 126) {
        frame.append(char(0x80 | n));
    } else if (n <= 0xFFFF) {
        frame.append(char(0x80 | 126));
        uchar len[2];
        qToBigEndian<quint16>(quint16(n), len);
        frame.append(reinterpret_cast<const char *>(len), 2);
    } else {
        frame.append(char(0x80 | 127));
        uchar len[8];
        qToBigEndian<quint64>(quint64(n), len);
        frame.append(reinterpret_cast<const char *>(len), 8);
    }
    const quint32 key = QRandomGenerator::global()->generate();
    uchar mask[4];
    memcpy(mask, &key, 4);
    frame.append(reinterpret_cast<const char *>(mask), 4);
    const qsizetype start = frame.size();
    frame.append(payload);
    char *p = frame.data() + start;
    for (qsizetype i = 0; i < n; ++i)
        p[i] = char(p[i] ^ mask[i % 4]);
    m_local->write(frame);
}
