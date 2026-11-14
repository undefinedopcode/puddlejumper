/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPoint>

class QTcpSocket;

/*
 * A VNC (RFB 3.3–3.8, RFC 6143) client: the connection and the remote
 * framebuffer, with no widgets.
 *
 * Kept to what a VM on the same machine needs, plus logging in to a Mac:
 * no authentication (QEMU's default), a VNC password, or Apple's (ARD)
 * account login; and raw and CopyRect updates, which are cheap
 * to decode and fast enough over a local link. It follows the remote
 * screen's size (DesktopSize, ExtendedDesktopSize) and cursor shape
 * (Cursor), and takes
 * keys as scan codes where the server can (QEMU's extended key events), so
 * the remote keyboard layout decides what a key types, not ours.
 */
class VncClient : public QObject
{
    Q_OBJECT

public:
    explicit VncClient(QObject *parent = nullptr);
    ~VncClient() override;

    void connectToHost(const QString &host, quint16 port);
    void disconnectFromHost();

    bool isConnected() const { return m_state == State::Normal || m_state == State::Rects; }
    /* The remote screen, as last updated. Null until connected. */
    const QImage &framebuffer() const { return m_image; }
    QString desktopName() const { return m_name; }
    /* Whether sendKey()'s scan code reaches the server (else only the keysym). */
    bool hasScanCodes() const { return m_scanCodes; }

    /*
     * What to log in with, for servers that ask: a VNC password (only its
     * first 8 characters count) or, for Apple's login, an account's name
     * and password. Asked for with credentialsRequested() when no password
     * was set; answering then carries on with the connection. The password
     * is forgotten once the server turns it down.
     */
    void setCredentials(const QString &username, const QString &password);
    /* The last connection ended because the login was turned down. */
    bool authenticationFailed() const { return m_authFailed; }

    /* `buttons`: 1 left, 2 middle, 4 right, 8/16 wheel up/down, 32/64 left/right. */
    void sendPointer(const QPoint &pos, int buttons);
    /*
     * A key going down or up, as an X keysym and an XT scan code (0xE0xx for
     * the extended ones). Either may be 0 when unknown; with scan codes the
     * server goes by the code, otherwise by the keysym.
     */
    void sendKey(bool down, quint32 keysym, quint32 scanCode);
    void sendClipboard(const QString &text);

signals:
    /* The server wants a login and no password is set: call setCredentials().
     * `needsUsername` for Apple's login, which takes an account name too. */
    void credentialsRequested(bool needsUsername);
    void connected();
    /* The framebuffer has a new size (and is blank until updated). */
    void resized(const QSize &size);
    void updated(const QRect &rect);
    /* A null image hides the cursor. */
    void cursorChanged(const QImage &image, const QPoint &hotspot);
    void bell();
    void clipboardReceived(const QString &text);
    /* `reason` is empty when the server just closed the connection. */
    void disconnected(const QString &reason);

private:
    enum class State {
        Idle, Version, SecurityTypes, SecurityType33, Challenge, ArdParameters,
        AwaitingCredentials, SecurityResult, ServerInit, Normal, Rects, Closed,
    };

    void onReadyRead();
    /* Handle one message (or part) from the buffer; false if more is needed. */
    bool parseOne();
    bool parseRect();
    void fail(const QString &reason);

    /* Answer the VNC challenge or Apple's key exchange. */
    void sendAuthentication();
    void sendChallengeResponse();
    void sendArdResponse();
    void sendClientInit();
    void sendSetup();
    void requestUpdate(bool incremental);
    void resizeFramebuffer(int width, int height);

    bool available(qsizetype n) const { return m_in.size() - m_pos >= n; }
    quint8 u8(qsizetype at) const;
    quint16 u16(qsizetype at) const;
    quint32 u32(qsizetype at) const;
    /* A length-prefixed string at `at`, if all of it is in; else null. */
    bool readString(qsizetype at, QString *out, qsizetype *size) const;

    QTcpSocket *m_socket = nullptr;
    State m_state = State::Idle;
    QByteArray m_in;
    qsizetype m_pos = 0;          // next unread byte of m_in
    int m_minor = 8;              // protocol 3.<minor>
    int m_rectsLeft = 0;          // in the FramebufferUpdate being read
    bool m_fullUpdate = false;    // ask for everything next (after a resize)
    bool m_scanCodes = false;
    QString m_username;
    QString m_password;           // null: ask for it
    int m_securityType = 0;       // as chosen: 1 none, 2 VNC, 30 Apple
    QByteArray m_challenge;       // VNC
    QByteArray m_ardGenerator;    // Apple: Diffie-Hellman parameters
    QByteArray m_ardPrime;
    QByteArray m_ardServerKey;
    bool m_authFailed = false;
    QImage m_image;
    QString m_name;
};
