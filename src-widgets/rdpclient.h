/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "remoteclient.h"

class RdpWorker;

/*
 * An RDP client, on FreeRDP: the connection and the remote framebuffer, with
 * no widgets. A SPIKE: enough to log in to a Windows machine, see its screen
 * and use its mouse and keyboard.
 *
 * FreeRDP runs on a thread of its own (RdpWorker) and draws into its own
 * buffer; what changed is copied over here, on the GUI thread, after each
 * update. RDP takes the login before it shows anything, so credentials are
 * always asked for first.
 *
 * A server's certificate is rarely one that can be verified (Windows makes
 * its own): it is asked about the first time, then remembered by its
 * fingerprint for that address (in the settings, rdp/certificates), and
 * asked about again, with a warning, if it ever differs.
 *
 * Not yet: the clipboard, resizing the remote desktop, sound.
 */
class RdpClient : public RemoteClient
{
    Q_OBJECT

public:
    explicit RdpClient(QObject *parent = nullptr);
    ~RdpClient() override;

    void connectToHost(const QString &host, quint16 port) override;
    void disconnectFromHost() override;

    bool isConnected() const override { return m_connected; }
    const QImage &framebuffer() const override { return m_image; }
    QString desktopName() const override { return m_host; }
    QString scheme() const override { return QStringLiteral("rdp"); }

    /* host or host:port (or [v6]:port), 3389 if not said. */
    static bool parseAddress(const QString &text, QString *host, quint16 *port);

    /* `username` may be DOMAIN\name. */
    void setCredentials(const QString &username, const QString &password) override;
    bool authenticationFailed() const override { return m_authFailed; }
    void answerCertificate(bool accept) override;

    void sendPointer(const QPoint &pos, int buttons) override;
    void sendKey(bool down, quint32 keysym, quint32 scanCode) override;
    void sendClipboard(const QString &) override {}

private:
    void stopWorker();
    QString certificateKey() const;

    RdpWorker *m_worker = nullptr;
    QString m_host;
    quint16 m_port = 3389;
    QImage m_image;
    bool m_connected = false;
    bool m_authFailed = false;
    int m_buttons = 0;
    QString m_askedFingerprint;
};
