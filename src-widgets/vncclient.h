/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "appleadaptive.h"
#include "applepasteboard.h"
#include "remoteclient.h"

#include <QByteArray>
#include <QImage>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPoint>

#include <memory>

class QTcpSocket;

/*
 * A VNC (RFB 3.3–3.8, RFC 6143) client: the connection and the remote
 * framebuffer, with no widgets.
 *
 * Kept to what a VM on the same machine needs, plus logging in to a Mac:
 * no authentication (QEMU's default), a VNC password, or Apple's (ARD)
 * account login. Over a network updates come Tight-compressed (JPEG for
 * what looks like a photo, zlib for the rest), else ZRLE or zlib, as a
 * Mac's Retina screen is tens of megabytes a frame raw; from this machine
 * they come raw, which is quicker there. CopyRect is used for moves. It follows the remote
 * screen's size (DesktopSize, ExtendedDesktopSize) and cursor shape
 * (Cursor), and takes
 * keys as scan codes where the server can (QEMU's extended key events), so
 * the remote keyboard layout decides what a key types, not ours.
 *
 * A Mac's Screen Sharing is spoken to in Apple's own revision, 003.889
 * (docs/apple-rfb-889.md): only there does it share its pasteboard, as
 * Unicode text here, and it pushes its updates unasked, takes a scroll as a
 * distance, and sends its cursors and its screen's size in forms of its own.
 */
class VncClient : public RemoteClient
{
    Q_OBJECT

public:
    explicit VncClient(QObject *parent = nullptr);
    ~VncClient() override;

    void connectToHost(const QString &host, quint16 port) override;
    void disconnectFromHost() override;

    bool isConnected() const override { return m_state == State::Normal || m_state == State::Rects; }
    /* The remote screen, as last updated. Null until connected. */
    const QImage &framebuffer() const override { return m_image; }
    QString scheme() const override { return QStringLiteral("vnc"); }
    QString desktopName() const override { return m_name; }
    /* Whether sendKey()'s scan code reaches the server (else only the keysym). */
    bool hasScanCodes() const { return m_scanCodes; }

    /*
     * Ask for 16-bit colour (RGB565) rather than 24: a third less to send,
     * for slow links, at the cost of banding in gradients and photos. Takes
     * effect from the next connection's setup.
     */
    void setReducedColour(bool on) override { m_reducedColour = on; }
    bool reducedColour() const override { return m_reducedColour; }
    /*
     * Ask a Mac for its Adaptive picture: coarse at once and sharpened as
     * the screen settles, for slow links. Only Apple's revision has it, at
     * full colour depth; any other server is unaffected. From the next
     * connection's setup.
     */
    void setAdaptiveQuality(bool on) override { m_adaptiveQuality = on; }
    bool adaptiveQuality() const override { return m_adaptiveQuality; }

    /*
     * What to log in with, for servers that ask: a VNC password (only its
     * first 8 characters count) or, for Apple's login, an account's name
     * and password. Asked for with credentialsRequested() when no password
     * was set; answering then carries on with the connection. The password
     * is forgotten once the server turns it down.
     */
    void setCredentials(const QString &username, const QString &password) override;
    /* The last connection ended because the login was turned down. */
    bool authenticationFailed() const override { return m_authFailed; }

    /* `buttons`: 1 left, 2 middle, 4 right, 8/16 wheel up/down, 32/64 left/right. */
    void sendPointer(const QPoint &pos, int buttons) override;
    bool sendScroll(const QPoint &pos, const QPoint &delta) override;
    /*
     * A key going down or up, as an X keysym and an XT scan code (0xE0xx for
     * the extended ones). Either may be 0 when unknown; with scan codes the
     * server goes by the code, otherwise by the keysym.
     */
    void sendKey(bool down, quint32 keysym, quint32 scanCode) override;
    void sendClipboard(const QString &text) override;

private:
    enum class State {
        Idle, Version, SecurityTypes, SecurityType33, Challenge, ArdParameters,
        AwaitingCredentials, SecurityResult, ServerInit, Normal, Rects, Pasteboard, Closed,
    };

    void open();
    void onReadyRead();
    /* Handle one message (or part) from the buffer; false if more is needed. */
    bool parseOne();
    bool parseRect();
    /* ZRLE's tiles, from the inflated data of one rectangle. */
    bool decodeZrle(const QByteArray &data, const QRect &rect);
    /* A Tight rectangle, whose header has been read; false if more is needed. */
    bool parseTight(const QRect &rect);
    /* Tight's 1-3 byte length at `at`; false if not all in yet. */
    bool readCompactLength(qsizetype at, quint32 *length, int *size) const;

    /* Pixels as the server sends them, as RGB32: whole ones (raw, zlib, the
     * cursor), ZRLE's and Tight's compacted ones, of compactPixelSize(). */
    quint32 pixel(const uchar *p) const;
    quint32 zrlePixel(const uchar *p) const;
    quint32 tightPixel(const uchar *p) const;
    int compactPixelSize() const { return m_bytesPerPixel == 4 ? 3 : 2; }
    void fail(const QString &reason);

    /* Answer the VNC challenge or Apple's key exchange. */
    void sendAuthentication();
    void sendChallengeResponse();
    void sendArdResponse();
    void sendClientInit();
    void sendSetup();
    void sendEncodings();
    void requestUpdate(bool incremental);
    /* Apple's revision: what follows ServerInit, and its own messages. */
    void sendApplePrelude();
    void armPushes();
    bool acceptsMessage(int type) const;
    void fetchPasteboard(bool promisesOnly);
    void finishPasteboard();
    bool parseAppleRect(qint32 encoding, const QRect &rect, bool *handled);
    void logStats();
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
    QString m_host;
    quint16 m_port = 0;
    bool m_apple = false;         // speaking Apple's 003.889
    bool m_plainOnly = false;     // not this time: the Mac offered no account login
    QByteArray m_appleAccepts;    // the client messages the Mac takes, a bit each
    bool m_pushed = false;        // the Mac sends updates unasked
    QString m_endReason;          // why the Mac is about to close, if it said
    // Cursor shapes come once and are then named by number.
    struct CursorShape { QImage image; QPoint hotspot; };
    QHash<quint32, CursorShape> m_cursors;
    // The pasteboard: what is asked for, and the reply being read.
    quint32 m_pasteCounter = 0;   // of the last fetch; replies to others are dropped
    quint32 m_pasteEcho = 0;
    bool m_pastePromises = false;
    qsizetype m_pasteLeft = 0;    // compressed bytes of the reply still to come
    std::unique_ptr<ApplePasteboard::Reader> m_pasteReader;
    QString m_macText;            // the text last known to be on the Mac's pasteboard
    bool m_adaptiveQuality = false;
    bool m_adaptiveWanted = false;   // for this connection
    bool m_adaptiveFailed = false;   // an update did not decode: no more of them
    std::unique_ptr<AppleAdaptive> m_adaptive;
    int m_rectsLeft = 0;          // in the FramebufferUpdate being read
    bool m_fullUpdate = false;    // ask for everything next (after a resize)
    qint64 m_fullOwed = 0;        // pixels of a whole picture asked for, yet to come
    qint64 m_paintedBefore = 0;   // m_fullOwed as this update began
    int m_fullAsks = 0;           // times left to ask a Mac for it again
    QElapsedTimer m_fullAsked;    // since it was asked for
    bool m_scanCodes = false;
    bool m_reducedColour = false;
    int m_bytesPerPixel = 4;      // as set up for this connection: 4, or 2 for 16-bit
    QString m_username;
    QString m_password;           // null: ask for it
    int m_securityType = 0;       // as chosen: 1 none, 2 VNC, 30 Apple
    QByteArray m_challenge;       // VNC
    QByteArray m_ardGenerator;    // Apple: Diffie-Hellman parameters
    QByteArray m_ardPrime;
    QByteArray m_ardServerKey;
    bool m_authFailed = false;

    // Each zlib-based encoding has one stream for the whole connection.
    struct Inflater;
    std::unique_ptr<Inflater> m_zrle;
    std::unique_ptr<Inflater> m_zlib;
    std::unique_ptr<Inflater> m_tight[4];   // Tight has four, which it can reset
    // What updates cost, logged every few seconds (puddlejumper.vnc).
    QElapsedTimer m_statsTimer;
    qint64 m_statsBytes = 0;
    qint64 m_statsPixels = 0;     // updated, to set against the screen's size
    qint64 m_statsDecodeNs = 0;
    int m_statsUpdates = 0;
    QHash<QString, int> m_statsRects;   // by encoding (and Tight's JPEG apart)

    QImage m_image;
    QString m_name;
};
