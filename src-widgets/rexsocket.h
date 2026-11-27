/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QLocalSocket;
class QWebSocket;

/*
 * Rex server addresses. A server is reached over TCP, http(s)://host:port,
 * or over a Unix socket, unix:///path/to/server.sock. Rex's own local
 * server listens only on its socket, $XDG_RUNTIME_DIR/rex/server.sock;
 * "unix:" alone means that one, and a bare absolute path a socket.
 */
namespace rexaddress {

// What was typed, as a server URL (http:// added to a bare host:port).
// Invalid when it can't be one.
QUrl parse(const QString &typed);
// A server URL that can be connected to: http(s) with a host, or unix with
// a socket path.
bool isValid(const QUrl &server);
bool isUnix(const QUrl &server);
// The socket's path, for a unix URL.
QString socketPath(const QUrl &server);
// Rex's default local socket ("" when there's no runtime directory).
QString defaultSocket();
// For messages: host:port, or the socket's path.
QString displayName(const QUrl &server);

} // namespace rexaddress

/*
 * A WebSocket to a Rex server, over TCP (QWebSocket) or a Unix socket.
 * QWebSocket can't use a Unix socket, so for one this is a small RFC 6455
 * client of its own over QLocalSocket: the HTTP upgrade (with the
 * subprotocol), masked frames out, fragments put together, pings answered,
 * and the close handshake.
 */
class RexSocket : public QObject
{
    Q_OBJECT

public:
    explicit RexSocket(QObject *parent = nullptr);
    ~RexSocket() override;

    // `server` is the server's address; `path` the endpoint ("/api/control").
    void open(const QUrl &server, const QString &path, const QString &subprotocol);
    void setMaxAllowedIncomingMessageSize(qint64 bytes) { m_maxMessage = bytes; }
    void sendTextMessage(const QString &text);
    void sendBinaryMessage(const QByteArray &data);
    void abort();
    // A clean close (the close handshake).
    void close();
    // Why it closed or failed: the server's close reason, else the error.
    QString failureReason() const;
    // The close code the server sent (1000 normal, ...), or 1006 when the
    // connection went without one. Meaningful once disconnected.
    int closeCode() const { return m_closeCode; }
    // Bytes given to send*Message not yet written to the network: for
    // sending a lot without holding it all in memory (see bytesWritten).
    qint64 bytesToWrite() const;

signals:
    void connected();
    void textMessageReceived(const QString &text);
    void binaryMessageReceived(const QByteArray &data);
    // Closed or failed (once, whichever comes first).
    void disconnected();
    // Some of what was sent has been written (bytesToWrite went down).
    void bytesWritten();

private:
    // Unix socket.
    void onLocalConnected();
    void onLocalData();
    void onLocalGone();
    bool readHandshake();
    void readFrames();
    void sendFrame(quint8 opcode, const QByteArray &payload);
    void finish(const QString &reason);

    QWebSocket *m_web = nullptr;
    QLocalSocket *m_local = nullptr;
    qint64 m_maxMessage = 16 << 20;
    QString m_path, m_subprotocol;
    QByteArray m_key;        // Sec-WebSocket-Key
    QByteArray m_buffer;     // read, not yet taken apart
    bool m_upgraded = false;
    QByteArray m_message;    // a fragmented message so far
    quint8 m_messageOpcode = 0;
    bool m_closing = false;  // our close frame sent
    bool m_done = false;
    QString m_reason;
    int m_closeCode = 1006;
    qint64 m_queued = 0;     // QWebSocket: sent, not yet written
};
