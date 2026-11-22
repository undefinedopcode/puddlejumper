/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QImage>
#include <QObject>
#include <QPoint>

/*
 * A connection to a remote screen, with no widgets: what the view (VncView)
 * needs of one, whatever the protocol. VncClient speaks VNC; RdpClient, RDP.
 */
class RemoteClient : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void connectToHost(const QString &host, quint16 port) = 0;
    virtual void disconnectFromHost() = 0;

    virtual bool isConnected() const = 0;
    /* The remote screen, as last updated. Null until connected. */
    virtual const QImage &framebuffer() const = 0;
    virtual QString desktopName() const = 0;
    /* The protocol, as in an address: "vnc" or "rdp". */
    virtual QString scheme() const = 0;

    /* 16-bit colour, for slow links; from the next connection. */
    virtual void setReducedColour(bool) {}
    virtual bool reducedColour() const { return false; }
    virtual void setAdaptiveQuality(bool) {}
    virtual bool adaptiveQuality() const { return false; }

    /* What to log in with; answers credentialsRequested(). */
    virtual void setCredentials(const QString &username, const QString &password) = 0;
    /* The last connection ended because the login was turned down. */
    virtual bool authenticationFailed() const = 0;
    /* Whether to go on with the server whose certificate was asked about. */
    virtual void answerCertificate(bool) {}

    /* `buttons`: 1 left, 2 middle, 4 right, 8/16 wheel up/down, 32/64 left/right. */
    virtual void sendPointer(const QPoint &pos, int buttons) = 0;
    /*
     * A scroll by a distance, in the remote screen's pixels (positive right
     * and down), for servers that take one. False if this one does not:
     * wheel clicks through sendPointer() are then all there is.
     */
    virtual bool sendScroll(const QPoint &pos, const QPoint &delta)
    {
        Q_UNUSED(pos);
        Q_UNUSED(delta);
        return false;
    }
    /* A key going down or up: an X keysym and an XT scan code (0xE0xx extended). */
    virtual void sendKey(bool down, quint32 keysym, quint32 scanCode) = 0;
    /*
     * The size the remote desktop should take, in pixels, and the scale it is
     * shown at (1: one of its pixels to a point), where the protocol can ask
     * for one (RDP); otherwise nothing happens, and the view scales.
     */
    virtual void setDesktopSize(const QSize &, qreal) {}
    virtual void sendClipboard(const QString &text) = 0;

signals:
    /* The server wants a login and no password is set: call setCredentials().
     * `needsUsername` when it takes an account name too. */
    void credentialsRequested(bool needsUsername);
    /*
     * The server's certificate can't be verified and isn't one trusted
     * before (`changed`: another was, for this address): the connection waits
     * for answerCertificate(). `fingerprint` is its SHA-256.
     */
    void certificateRequested(const QString &name, const QString &fingerprint, bool changed);
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
};
