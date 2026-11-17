/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rdpclient.h"

#include <QCryptographicHash>
#include <QMutex>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <freerdp/freerdp.h>
#include <freerdp/codec/color.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>

/*
 * The connection, on its own thread: FreeRDP's event loop, and its calls
 * back. Talks to the RdpClient with (queued) signals; input comes the other
 * way through send*(), which FreeRDP takes from any thread.
 */
class RdpWorker : public QThread
{
    Q_OBJECT

public:
    QString host;
    quint16 port = 3389;
    QString username, domain, password;

    void stop()
    {
        QMutexLocker lock(&m_mutex);
        m_stopping = true;
        m_certificateAnswered.wakeAll();
        if (m_instance)
            freerdp_abort_connect_context(m_instance->context);
    }
    void answerCertificate(bool accept)
    {
        QMutexLocker lock(&m_mutex);
        m_certificateAnswer = accept ? 1 : 0;
        m_certificateAnswered.wakeAll();
    }
    void sendMouse(quint16 flags, const QPoint &pos)
    {
        QMutexLocker lock(&m_mutex);
        if (m_instance && m_up)
            freerdp_input_send_mouse_event(m_instance->context->input, flags,
                                           quint16(qMax(0, pos.x())), quint16(qMax(0, pos.y())));
    }
    void sendKeyboard(quint16 flags, quint8 code)
    {
        QMutexLocker lock(&m_mutex);
        if (m_instance && m_up)
            freerdp_input_send_keyboard_event(m_instance->context->input, flags, code);
    }

signals:
    void up();
    void resized(const QSize &size);
    /* What changed: a copy of that part of the screen, and where it goes. */
    void painted(const QImage &part, const QPoint &at);
    /* The pointer's shape; null for the usual one, empty (transparent) for none. */
    void cursor(const QImage &image, const QPoint &hotspot);
    /* The server's certificate is to be judged: answerCertificate(). */
    void certificate(const QString &name, const QString &fingerprint);
    void down(const QString &reason, bool loginRefused);

protected:
    void run() override;

private:
    struct Context {
        rdpContext context;   // first: FreeRDP's own
        RdpWorker *worker;
    };
    static RdpWorker *of(rdpContext *context) { return reinterpret_cast<Context *>(context)->worker; }
    struct Pointer {
        rdpPointer pointer;   // first: FreeRDP's own
        QImage *image;
    };

    /* Asks, and waits for the answer: FreeRDP's (2: for this connection; 0: no). */
    DWORD judgeCertificate(const char *name, const char *fingerprint, DWORD flags);

    static BOOL postConnect(freerdp *instance);
    static void postDisconnect(freerdp *instance);
    static BOOL beginPaint(rdpContext *context);
    static BOOL endPaint(rdpContext *context);
    static BOOL desktopResize(rdpContext *context);

    QMutex m_mutex;
    freerdp *m_instance = nullptr;
    bool m_up = false;
    bool m_stopping = false;
    QWaitCondition m_certificateAnswered;
    int m_certificateAnswer = -1;
    bool m_certificateRefused = false;
};

DWORD RdpWorker::judgeCertificate(const char *name, const char *fingerprint, DWORD flags)
{
    // The SHA-256 of the certificate, however it was handed over.
    QString sha256 = QString::fromLatin1(fingerprint).trimmed();
    if (flags & VERIFY_CERT_FLAG_FP_IS_PEM) {
        static const QRegularExpression armour(QStringLiteral("-----[^-]+-----|\\s"));
        const QByteArray der = QByteArray::fromBase64(sha256.remove(armour).toLatin1());
        sha256 = QString::fromLatin1(
            QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex(':'));
    }
    QMutexLocker lock(&m_mutex);
    if (m_stopping)
        return 0;
    m_certificateAnswer = -1;
    lock.unlock();
    emit certificate(QString::fromUtf8(name), sha256.toLower());
    lock.relock();
    while (m_certificateAnswer < 0 && !m_stopping)
        m_certificateAnswered.wait(&m_mutex);
    m_certificateRefused = m_certificateAnswer <= 0;
    return m_certificateRefused ? 0 : 2;
}

BOOL RdpWorker::postConnect(freerdp *instance)
{
    if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
        return FALSE;
    rdpUpdate *update = instance->context->update;
    update->BeginPaint = beginPaint;
    update->EndPaint = endPaint;
    update->DesktopResize = desktopResize;

    // The pointer's shapes, as images.
    rdpPointer pointer = {};
    pointer.size = sizeof(Pointer);
    pointer.New = [](rdpContext *context, rdpPointer *p) -> BOOL {
        auto *image = new QImage(int(p->width), int(p->height), QImage::Format_ARGB32);
        image->fill(Qt::transparent);
        reinterpret_cast<Pointer *>(p)->image = image;
        if (image->isNull())
            return TRUE;
        return freerdp_image_copy_from_pointer_data(
            image->bits(), PIXEL_FORMAT_BGRA32, UINT32(image->bytesPerLine()), 0, 0, p->width,
            p->height, p->xorMaskData, p->lengthXorMask, p->andMaskData, p->lengthAndMask,
            p->xorBpp, &context->gdi->palette);
    };
    pointer.Free = [](rdpContext *, rdpPointer *p) {
        delete reinterpret_cast<Pointer *>(p)->image;
        reinterpret_cast<Pointer *>(p)->image = nullptr;
    };
    pointer.Set = [](rdpContext *context, rdpPointer *p) -> BOOL {
        const QImage *image = reinterpret_cast<Pointer *>(p)->image;
        if (image && !image->isNull())
            emit of(context)->cursor(image->copy(), QPoint(int(p->xPos), int(p->yPos)));
        return TRUE;
    };
    pointer.SetNull = [](rdpContext *context) -> BOOL {
        QImage none(1, 1, QImage::Format_ARGB32);
        none.fill(Qt::transparent);
        emit of(context)->cursor(none, QPoint());
        return TRUE;
    };
    pointer.SetDefault = [](rdpContext *context) -> BOOL {
        emit of(context)->cursor(QImage(), QPoint());
        return TRUE;
    };
    pointer.SetPosition = [](rdpContext *, UINT32, UINT32) -> BOOL { return TRUE; };
    graphics_register_pointer(instance->context->graphics, &pointer);
    const rdpGdi *gdi = instance->context->gdi;
    emit of(instance->context)->resized(QSize(gdi->width, gdi->height));
    return TRUE;
}

void RdpWorker::postDisconnect(freerdp *instance)
{
    gdi_free(instance);
}

BOOL RdpWorker::beginPaint(rdpContext *context)
{
    context->gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

BOOL RdpWorker::endPaint(rdpContext *context)
{
    const rdpGdi *gdi = context->gdi;
    const GDI_RGN *invalid = gdi->primary->hdc->hwnd->invalid;
    if (invalid->null)
        return TRUE;
    const QRect rect = QRect(invalid->x, invalid->y, invalid->w, invalid->h)
                           .intersected(QRect(0, 0, gdi->width, gdi->height));
    if (rect.isEmpty())
        return TRUE;
    // A view on FreeRDP's buffer, then a copy of the part, to hand over.
    const QImage whole(gdi->primary_buffer, gdi->width, gdi->height, int(gdi->stride),
                       QImage::Format_RGB32);
    emit of(context)->painted(whole.copy(rect), rect.topLeft());
    return TRUE;
}

BOOL RdpWorker::desktopResize(rdpContext *context)
{
    const rdpSettings *settings = context->settings;
    const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
    const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
    if (!gdi_resize(context->gdi, width, height))
        return FALSE;
    emit of(context)->resized(QSize(int(width), int(height)));
    return TRUE;
}

void RdpWorker::run()
{
    // FreeRDP says a great deal on stderr; SUPERTERM_RDP_LOG (a level: debug,
    // info, warn...) lets it.
    static const bool quiet = []() {
        const QByteArray level = qgetenv("SUPERTERM_RDP_LOG");
        WLog_SetStringLogLevel(WLog_GetRoot(), level.isEmpty() ? "OFF" : level.constData());
        return true;
    }();
    Q_UNUSED(quiet);
    freerdp *instance = freerdp_new();
    instance->ContextSize = sizeof(Context);
    instance->PostConnect = postConnect;
    instance->PostDisconnect = postDisconnect;
    // No asking from in here: a login that is refused ends the connection,
    // and the view asks again.
    instance->AuthenticateEx = [](freerdp *, char **, char **, char **, rdp_auth_reason) -> BOOL {
        return FALSE;
    };
    // A certificate FreeRDP can't verify is ours to judge (see RdpClient):
    // never "for good" as far as FreeRDP goes, so that it keeps asking, and
    // the remembering is ours alone.
    instance->VerifyCertificateEx = [](freerdp *instance, const char *, UINT16,
                                       const char *commonName, const char *, const char *,
                                       const char *fingerprint, DWORD flags) -> DWORD {
        return of(instance->context)->judgeCertificate(commonName, fingerprint, flags);
    };
    instance->VerifyChangedCertificateEx = [](freerdp *instance, const char *, UINT16,
                                              const char *commonName, const char *, const char *,
                                              const char *fingerprint, const char *, const char *,
                                              const char *, DWORD flags) -> DWORD {
        return of(instance->context)->judgeCertificate(commonName, fingerprint, flags);
    };
    if (!freerdp_context_new(instance)) {
        freerdp_free(instance);
        emit down(tr("FreeRDP could not start."), false);
        return;
    }
    rdpContext *context = instance->context;
    reinterpret_cast<Context *>(context)->worker = this;

    rdpSettings *settings = context->settings;
    freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host.toUtf8().constData());
    freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port);
    freerdp_settings_set_string(settings, FreeRDP_Username, username.toUtf8().constData());
    freerdp_settings_set_string(settings, FreeRDP_Password, password.toUtf8().constData());
    if (!domain.isEmpty())
        freerdp_settings_set_string(settings, FreeRDP_Domain, domain.toUtf8().constData());
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 1600);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 900);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    bool stopping;
    {
        QMutexLocker lock(&m_mutex);
        m_instance = instance;
        stopping = m_stopping;
    }

    QString reason;
    bool loginRefused = false;
    if (!stopping && freerdp_connect(instance)) {
        {
            QMutexLocker lock(&m_mutex);
            m_up = true;
        }
        emit up();
        while (!freerdp_shall_disconnect_context(context)) {
            HANDLE handles[MAXIMUM_WAIT_OBJECTS];
            const DWORD count = freerdp_get_event_handles(context, handles, MAXIMUM_WAIT_OBJECTS);
            if (count == 0 || WaitForMultipleObjects(count, handles, FALSE, 250) == WAIT_FAILED)
                break;
            if (!freerdp_check_event_handles(context))
                break;
        }
    }
    const UINT32 error = freerdp_get_last_error(context);
    if (error != FREERDP_ERROR_SUCCESS && error != FREERDP_ERROR_CONNECT_CANCELLED) {
        reason = QString::fromUtf8(freerdp_get_last_error_string(error));
        loginRefused = error == FREERDP_ERROR_AUTHENTICATION_FAILED
                       || error == FREERDP_ERROR_CONNECT_LOGON_FAILURE
                       || error == FREERDP_ERROR_CONNECT_WRONG_PASSWORD
                       || error == FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS;
    }
    if (m_certificateRefused)
        reason = tr("Its certificate was not trusted.");
    {
        QMutexLocker lock(&m_mutex);
        m_up = false;
        m_instance = nullptr;
    }
    freerdp_disconnect(instance);
    freerdp_context_free(instance);
    freerdp_free(instance);
    emit down(reason, loginRefused);
}

RdpClient::RdpClient(QObject *parent)
    : RemoteClient(parent)
{
}

RdpClient::~RdpClient()
{
    stopWorker();
}

void RdpClient::stopWorker()
{
    if (!m_worker)
        return;
    m_worker->disconnect(this);
    m_worker->stop();
    m_worker->wait();
    delete m_worker;
    m_worker = nullptr;
    m_connected = false;
}

bool RdpClient::parseAddress(const QString &text, QString *host, quint16 *port)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*(?:\[([^\]]+)\]|([^:\s]+))(?::(\d+))?\s*$)"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return false;
    const int n = m.captured(3).isEmpty() ? 3389 : m.captured(3).toInt();
    if (n <= 0 || n > 65535)
        return false;
    *host = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
    *port = quint16(n);
    return true;
}

QString RdpClient::certificateKey() const
{
    return QStringLiteral("rdp/certificates/%1_%2").arg(m_host).arg(m_port);
}

void RdpClient::answerCertificate(bool accept)
{
    if (accept && !m_askedFingerprint.isEmpty())
        QSettings().setValue(certificateKey(), m_askedFingerprint);
    m_askedFingerprint.clear();
    if (m_worker)
        m_worker->answerCertificate(accept);
}

void RdpClient::connectToHost(const QString &host, quint16 port)
{
    stopWorker();
    m_host = host;
    m_port = port;
    // The login comes first in RDP: nothing to connect with until it is given.
    QTimer::singleShot(0, this, [this]() { emit credentialsRequested(true); });
}

void RdpClient::disconnectFromHost()
{
    const bool was = m_worker != nullptr;
    stopWorker();
    if (was)
        emit disconnected(QString());
}

void RdpClient::setCredentials(const QString &username, const QString &password)
{
    stopWorker();
    m_authFailed = false;
    m_buttons = 0;

    m_worker = new RdpWorker;
    m_worker->host = m_host;
    m_worker->port = m_port;
    const int slash = username.indexOf(QLatin1Char('\\'));
    m_worker->domain = slash > 0 ? username.left(slash) : QString();
    m_worker->username = username.mid(slash + 1);
    m_worker->password = password;

    connect(m_worker, &RdpWorker::up, this, [this]() {
        m_connected = true;
        emit connected();
    });
    connect(m_worker, &RdpWorker::resized, this, [this](const QSize &size) {
        m_image = QImage(size, QImage::Format_RGB32);
        m_image.fill(Qt::black);
        emit resized(size);
    });
    connect(m_worker, &RdpWorker::painted, this, [this](const QImage &part, const QPoint &at) {
        const QRect rect = QRect(at, part.size()).intersected(m_image.rect());
        if (rect.isEmpty())
            return;   // from before a resize
        const int bytes = rect.width() * 4;
        for (int y = 0; y < rect.height(); ++y)
            memcpy(m_image.scanLine(rect.y() + y) + rect.x() * 4, part.constScanLine(y), bytes);
        emit updated(rect);
    });
    connect(m_worker, &RdpWorker::cursor, this, &RdpClient::cursorChanged);
    connect(m_worker, &RdpWorker::certificate, this,
            [this](const QString &name, const QString &fingerprint) {
        const QString trusted = QSettings().value(certificateKey()).toString();
        if (trusted == fingerprint) {
            m_worker->answerCertificate(true);
            return;
        }
        m_askedFingerprint = fingerprint;
        emit certificateRequested(name, fingerprint, !trusted.isEmpty());
    });
    connect(m_worker, &RdpWorker::down, this, [this](const QString &reason, bool loginRefused) {
        m_worker->wait();
        m_worker->deleteLater();
        m_worker = nullptr;
        m_connected = false;
        m_authFailed = loginRefused;
        emit disconnected(reason);
    });
    m_worker->start();
}

void RdpClient::sendPointer(const QPoint &pos, int buttons)
{
    if (!m_worker || !m_connected)
        return;
    const int changed = buttons ^ m_buttons;
    m_buttons = buttons;
    static const struct { int bit; quint16 flag; } kButtons[] = {
        {1, PTR_FLAGS_BUTTON1}, {2, PTR_FLAGS_BUTTON3}, {4, PTR_FLAGS_BUTTON2},
    };
    bool sent = false;
    for (const auto &b : kButtons) {
        if (changed & b.bit) {
            m_worker->sendMouse(b.flag | ((buttons & b.bit) ? PTR_FLAGS_DOWN : 0), pos);
            sent = true;
        }
    }
    // The wheel: a notch (120) when its "button" goes down; 9 bits, signed.
    if (changed & buttons & 8) {
        m_worker->sendMouse(PTR_FLAGS_WHEEL | 120, pos);
        sent = true;
    }
    if (changed & buttons & 16) {
        m_worker->sendMouse(PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | (0x100 - 120), pos);
        sent = true;
    }
    if (changed & buttons & 32) {
        m_worker->sendMouse(PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | (0x100 - 120), pos);
        sent = true;
    }
    if (changed & buttons & 64) {
        m_worker->sendMouse(PTR_FLAGS_HWHEEL | 120, pos);
        sent = true;
    }
    if (!sent)
        m_worker->sendMouse(PTR_FLAGS_MOVE, pos);
}

void RdpClient::sendKey(bool down, quint32, quint32 scanCode)
{
    // By scan code only: RDP has no keysyms (Unicode input aside).
    if (!m_worker || !m_connected || scanCode == 0)
        return;
    quint16 flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
    if ((scanCode & 0xff00) == 0xe000)
        flags |= KBD_FLAGS_EXTENDED;
    m_worker->sendKeyboard(flags, quint8(scanCode & 0xff));
}

#include "rdpclient.moc"
