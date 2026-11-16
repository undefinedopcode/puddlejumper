/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "vncclient.h"

#include "vnccrypto.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTcpSocket>
#include <QtEndian>

#include <cstring>

#include <zlib.h>

// The connection's steps, for when a server won't have us, and what updates
// cost: QT_LOGGING_RULES="superterm.vnc.debug=true". SUPERTERM_VNC_ENCODINGS
// (e.g. "raw,copyrect", or "tight,jpeg" for Tight with JPEG) limits the
// encodings asked for, to compare them.
Q_LOGGING_CATEGORY(lcVnc, "superterm.vnc", QtInfoMsg)

namespace {

// Encodings (and pseudo-encodings) we ask for, in order of preference.
constexpr qint32 kEncRaw = 0;
constexpr qint32 kEncCopyRect = 1;
constexpr qint32 kEncZlib = 6;
constexpr qint32 kEncTight = 7;
constexpr qint32 kEncZrle = 16;
constexpr qint32 kEncJpegQuality = -32 + 7;   // Tight's JPEG quality, level 7 of 0-9
constexpr qint32 kEncDesktopSize = -223;
constexpr qint32 kEncCursor = -239;
constexpr qint32 kEncQemuKeys = -258;
constexpr qint32 kEncExtendedDesktopSize = -308;

constexpr quint32 kMaxText = 16 * 1024 * 1024;   // cut text and reasons

// Security types.
constexpr int kSecNone = 1;
constexpr int kSecVnc = 2;
constexpr int kSecArd = 30;

QByteArray randomBytes(int count)
{
    QByteArray out(count, '\0');
    for (char &c : out)
        c = char(QRandomGenerator::system()->bounded(256));
    return out;
}

QString encodingName(qint32 encoding)
{
    switch (encoding) {
    case kEncRaw: return QStringLiteral("raw");
    case kEncCopyRect: return QStringLiteral("copyrect");
    case kEncZlib: return QStringLiteral("zlib");
    case kEncTight: return QStringLiteral("tight");
    case kEncZrle: return QStringLiteral("zrle");
    default: return QString::number(encoding);
    }
}

// RGB565's channels, widened to eight bits each.
quint32 fromRgb565(int r, int g, int b)
{
    return 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8)
           | ((b << 3) | (b >> 2));
}

quint32 fromRgb565(quint16 v)
{
    return fromRgb565((v >> 11) & 31, (v >> 5) & 63, v & 31);
}

void put8(QByteArray &out, quint8 v) { out.append(char(v)); }
void put16(QByteArray &out, quint16 v)
{
    const quint16 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 2);
}
void put32(QByteArray &out, quint32 v)
{
    const quint32 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 4);
}

} // namespace

/*
 * A zlib stream that lasts the connection: each rectangle's data carries on
 * from the last one's.
 */
struct VncClient::Inflater
{
    z_stream stream{};
    bool ok = false;

    Inflater() { ok = inflateInit(&stream) == Z_OK; }
    ~Inflater()
    {
        if (ok)
            inflateEnd(&stream);
    }

    /* All of `in`, inflated; false if the stream is broken. */
    bool inflate(const char *in, qsizetype size, QByteArray *out, qsizetype expected = 0)
    {
        if (!ok)
            return false;
        out->resize(qMax<qsizetype>(expected, size * 4 + 1024));
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in));
        stream.avail_in = uInt(size);
        qsizetype produced = 0;
        while (true) {
            stream.next_out = reinterpret_cast<Bytef *>(out->data() + produced);
            stream.avail_out = uInt(out->size() - produced);
            const int result = ::inflate(&stream, Z_SYNC_FLUSH);
            produced = out->size() - stream.avail_out;
            if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END)
                return false;
            if (result == Z_STREAM_END || (stream.avail_in == 0 && stream.avail_out > 0))
                break;
            if (result == Z_BUF_ERROR && stream.avail_in == 0)
                break;
            out->resize(out->size() * 2);   // out of room: more
        }
        out->resize(produced);
        return true;
    }
};

VncClient::VncClient(QObject *parent)
    : RemoteClient(parent), m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_state = State::Version;
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &VncClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this]() {
        if (m_state != State::Closed)
            fail(m_socket->error() == QAbstractSocket::RemoteHostClosedError
                     ? QString() : m_socket->errorString());
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        if (m_state != State::Closed)
            fail(QString());
    });
}

VncClient::~VncClient()
{
    // No signals from a half-destroyed object.
    m_socket->disconnect(this);
}

void VncClient::connectToHost(const QString &host, quint16 port)
{
    m_socket->abort();
    m_in.clear();
    m_pos = 0;
    m_scanCodes = false;
    m_securityType = 0;
    m_zrle = std::make_unique<Inflater>();
    m_zlib = std::make_unique<Inflater>();
    for (auto &stream : m_tight)
        stream = std::make_unique<Inflater>();
    m_statsTimer.start();
    m_statsBytes = 0;
    m_statsPixels = 0;
    m_statsDecodeNs = 0;
    m_statsUpdates = 0;
    m_statsRects.clear();
    m_authFailed = false;
    m_state = State::Idle;
    m_socket->connectToHost(host, port);
}

void VncClient::disconnectFromHost()
{
    m_state = State::Closed;
    m_socket->abort();
}

void VncClient::fail(const QString &reason)
{
    m_state = State::Closed;
    m_socket->abort();
    emit disconnected(reason);
}

quint8 VncClient::u8(qsizetype at) const
{
    return quint8(m_in.at(m_pos + at));
}

quint16 VncClient::u16(qsizetype at) const
{
    return qFromBigEndian<quint16>(m_in.constData() + m_pos + at);
}

quint32 VncClient::u32(qsizetype at) const
{
    return qFromBigEndian<quint32>(m_in.constData() + m_pos + at);
}

bool VncClient::readString(qsizetype at, QString *out, qsizetype *size) const
{
    if (!available(at + 4))
        return false;
    const quint32 length = qMin(u32(at), kMaxText);
    if (!available(at + 4 + length))
        return false;
    *out = QString::fromUtf8(m_in.constData() + m_pos + at + 4, length);
    *size = 4 + length;
    return true;
}

void VncClient::onReadyRead()
{
    const QByteArray data = m_socket->readAll();
    m_statsBytes += data.size();
    m_in.append(data);
    while (m_state != State::Closed && parseOne()) {
    }
    m_in.remove(0, m_pos);
    m_pos = 0;
}

bool VncClient::parseOne()
{
    switch (m_state) {
    case State::Idle:
    case State::Closed:
        return false;

    case State::Version: {
        if (!available(12))
            return false;
        const QByteArray version = m_in.mid(m_pos, 12);
        m_pos += 12;
        if (!version.startsWith("RFB 003.")) {
            fail(tr("Not a VNC server"));
            return false;
        }
        const int minor = version.mid(8, 3).toInt();
        m_minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;
        qCDebug(lcVnc) << "server version" << version.trimmed() << "-> speaking 3." << m_minor;
        m_socket->write(QByteArrayLiteral("RFB 003.00") + QByteArray::number(m_minor) + '\n');
        m_state = m_minor == 3 ? State::SecurityType33 : State::SecurityTypes;
        return true;
    }

    case State::SecurityTypes: {
        if (!available(1))
            return false;
        const int count = u8(0);
        if (count == 0) {
            QString reason;
            qsizetype size;
            if (!readString(1, &reason, &size))
                return false;
            fail(reason);
            return false;
        }
        if (!available(1 + count))
            return false;
        QList<int> offered;
        for (int i = 0; i < count; ++i)
            offered << u8(1 + i);
        m_pos += 1 + count;
        // None if allowed; then Apple's login over the VNC password, as a
        // Mac offers both and its account is the stronger of the two.
        for (int type : {kSecNone, kSecArd, kSecVnc}) {
            if (offered.contains(type)) {
                m_securityType = type;
                break;
            }
        }
        qCDebug(lcVnc) << "security types offered" << offered << "chose" << m_securityType;
        if (!m_securityType) {
            fail(tr("The server offers no supported security type"));
            return false;
        }
        QByteArray choice;
        put8(choice, quint8(m_securityType));
        m_socket->write(choice);
        if (m_securityType == kSecVnc)
            m_state = State::Challenge;
        else if (m_securityType == kSecArd)
            m_state = State::ArdParameters;
        else if (m_minor >= 8)
            m_state = State::SecurityResult;
        else
            sendClientInit();   // 3.7 only sends a result for types that authenticate
        return true;
    }

    case State::SecurityType33: {
        if (!available(4))
            return false;
        const quint32 type = u32(0);
        if (type == 0) {
            QString reason;
            qsizetype size;
            if (!readString(4, &reason, &size))
                return false;
            fail(reason);
            return false;
        }
        m_pos += 4;
        if (type == kSecVnc) {
            m_securityType = kSecVnc;
            m_state = State::Challenge;
            return true;
        }
        if (type != 1) {
            fail(tr("The server offers no supported security type"));
            return false;
        }
        sendClientInit();
        return true;
    }

    case State::Challenge:
        if (!available(16))
            return false;
        m_challenge = m_in.mid(m_pos, 16);
        m_pos += 16;
        sendAuthentication();
        return true;

    case State::ArdParameters: {
        // Generator, key length, then the prime and the server's public key.
        if (!available(4))
            return false;
        const int keyLength = u16(2);
        if (!available(4 + 2 * keyLength))
            return false;
        m_ardGenerator = m_in.mid(m_pos, 2);
        m_ardPrime = m_in.mid(m_pos + 4, keyLength);
        m_ardServerKey = m_in.mid(m_pos + 4 + keyLength, keyLength);
        m_pos += 4 + 2 * keyLength;
        qCDebug(lcVnc) << "ARD: generator" << m_ardGenerator.toHex() << "key length" << keyLength;
        sendAuthentication();
        return true;
    }

    case State::AwaitingCredentials:
        return false;

    case State::SecurityResult: {
        if (!available(4))
            return false;
        if (u32(0) != 0) {
            // Only 3.8 says why.
            QString reason;
            qsizetype size;
            if (m_minor >= 8 && !readString(4, &reason, &size))
                return false;
            qCDebug(lcVnc) << "security result" << u32(0) << "reason" << reason;
            m_authFailed = m_securityType != kSecNone;
            if (m_authFailed)
                m_password = QString();   // never sent again
            if (reason.isEmpty())
                reason = m_securityType == kSecArd ? tr("Wrong user name or password")
                         : m_authFailed            ? tr("Wrong password")
                                                   : tr("The server refused the connection");
            fail(reason);
            return false;
        }
        m_pos += 4;
        sendClientInit();
        return true;
    }

    case State::ServerInit: {
        // Width, height, the server's pixel format (replaced by ours), name.
        QString name;
        qsizetype nameSize;
        if (!readString(20, &name, &nameSize))
            return false;
        const int width = u16(0), height = u16(2);
        m_pos += 20 + nameSize;
        m_name = name;
        m_state = State::Normal;
        sendSetup();
        resizeFramebuffer(width, height);
        requestUpdate(false);
        m_fullUpdate = false;
        emit connected();
        return true;
    }

    case State::Normal: {
        if (!available(1))
            return false;
        switch (u8(0)) {
        case 0:   // FramebufferUpdate
            if (!available(4))
                return false;
            m_rectsLeft = u16(2);
            m_pos += 4;
            m_state = State::Rects;
            // Ask for the next one now, so the server works on it while
            // this one downloads, rather than after: a round trip less per
            // update. (After a resize the full one is asked for at the end.)
            requestUpdate(true);
            if (m_rectsLeft == 0)
                m_state = State::Normal;
            return true;
        case 1: {   // SetColourMapEntries: not used with a true-colour format
            if (!available(6))
                return false;
            const qsizetype size = 6 + 6 * qsizetype(u16(4));
            if (!available(size))
                return false;
            m_pos += size;
            return true;
        }
        case 2:   // Bell
            m_pos += 1;
            emit bell();
            return true;
        case 3: {   // ServerCutText (Latin-1)
            if (!available(8))
                return false;
            const quint32 length = u32(4);
            if (length > kMaxText) {
                fail(tr("Clipboard text too large"));
                return false;
            }
            if (!available(8 + length))
                return false;
            const QString text = QString::fromLatin1(m_in.constData() + m_pos + 8, length);
            m_pos += 8 + length;
            emit clipboardReceived(text);
            return true;
        }
        default:
            fail(tr("Unknown message %1 from the server").arg(u8(0)));
            return false;
        }
    }

    case State::Rects:
        if (!parseRect())
            return false;
        if (--m_rectsLeft == 0) {
            m_state = State::Normal;
            if (m_fullUpdate)
                requestUpdate(false);
            m_fullUpdate = false;
            ++m_statsUpdates;
            if (m_statsTimer.elapsed() >= 5000)
                logStats();
        }
        return true;
    }
    return false;
}

void VncClient::logStats()
{
    const qreal seconds = m_statsTimer.restart() / 1000.0;
    const qreal screen = qMax<qreal>(1, qreal(m_image.width()) * m_image.height());
    qCDebug(lcVnc).nospace()
        << m_statsUpdates / seconds << " updates/s, " << m_statsBytes / seconds / 1e6
        << " MB/s, " << m_statsPixels / seconds / screen << " screens/s, decoding "
        << m_statsDecodeNs / seconds / 1e6 << " ms/s; rectangles by encoding " << m_statsRects;
    m_statsBytes = 0;
    m_statsPixels = 0;
    m_statsDecodeNs = 0;
    m_statsUpdates = 0;
    m_statsRects.clear();
}

bool VncClient::parseRect()
{
    if (!available(12))
        return false;
    const int x = u16(0), y = u16(2), w = u16(4), h = u16(6);
    const qint32 encoding = qint32(u32(8));
    const QRect rect(x, y, w, h);
    QElapsedTimer decoding;
    decoding.start();
    const bool inside = m_image.rect().contains(rect) || rect.isEmpty();

    switch (encoding) {
    case kEncTight:
        if (!inside) {
            fail(tr("Update outside the screen"));
            return false;
        }
        if (!parseTight(rect))
            return false;
        m_statsPixels += qint64(w) * h;
        m_statsDecodeNs += decoding.nsecsElapsed();
        emit updated(rect);
        return true;

    case kEncZrle:
    case kEncZlib: {
        if (!available(16))
            return false;
        const quint32 length = u32(12);
        if (!available(16 + qsizetype(length)))
            return false;
        if (!inside) {
            fail(tr("Update outside the screen"));
            return false;
        }
        QByteArray data;
        Inflater &inflater = encoding == kEncZrle ? *m_zrle : *m_zlib;
        const qsizetype raw = qsizetype(w) * h * m_bytesPerPixel;
        if (!inflater.inflate(m_in.constData() + m_pos + 16, length, &data,
                              encoding == kEncZlib ? raw : 0)) {
            fail(tr("Corrupt compressed update"));
            return false;
        }
        m_pos += 16 + length;
        ++m_statsRects[encodingName(encoding)];
        m_statsPixels += qint64(w) * h;
        if (encoding == kEncZrle) {
            if (!decodeZrle(data, rect)) {
                fail(tr("Corrupt ZRLE update"));
                return false;
            }
        } else {
            // zlib: raw pixels, compressed.
            if (data.size() < raw) {
                fail(tr("Corrupt zlib update"));
                return false;
            }
            const auto *src = reinterpret_cast<const uchar *>(data.constData());
            for (int row = 0; row < h; ++row) {
                auto *dst = reinterpret_cast<quint32 *>(m_image.scanLine(y + row)) + x;
                for (int col = 0; col < w; ++col, src += m_bytesPerPixel)
                    dst[col] = pixel(src);
            }
        }
        m_statsDecodeNs += decoding.nsecsElapsed();
        emit updated(rect);
        return true;
    }

    case kEncRaw: {
        const qsizetype size = 12 + qsizetype(w) * h * m_bytesPerPixel;
        if (!available(size))
            return false;
        if (!inside) {
            fail(tr("Update outside the screen"));
            return false;
        }
        const auto *src = reinterpret_cast<const uchar *>(m_in.constData() + m_pos + 12);
        for (int row = 0; row < h; ++row) {
            auto *dst = reinterpret_cast<quint32 *>(m_image.scanLine(y + row)) + x;
            for (int col = 0; col < w; ++col, src += m_bytesPerPixel)
                dst[col] = pixel(src);
        }
        m_pos += size;
        ++m_statsRects[encodingName(encoding)];
        m_statsPixels += qint64(w) * h;
        m_statsDecodeNs += decoding.nsecsElapsed();
        emit updated(rect);
        return true;
    }

    case kEncCopyRect: {
        if (!available(16))
            return false;
        const QRect from(u16(12), u16(14), w, h);
        m_pos += 16;
        if (!inside || !m_image.rect().contains(from)) {
            fail(tr("Update outside the screen"));
            return false;
        }
        // Through a copy, as the two may overlap.
        const QImage part = m_image.copy(from);
        for (int row = 0; row < h; ++row)
            std::memcpy(m_image.scanLine(y + row) + x * 4, part.constScanLine(row), w * 4);
        emit updated(rect);
        return true;
    }

    case kEncDesktopSize:
        m_pos += 12;
        resizeFramebuffer(w, h);
        return true;

    case kEncExtendedDesktopSize: {
        if (!available(16))
            return false;
        const qsizetype size = 16 + 16 * qsizetype(u8(12));
        if (!available(size))
            return false;
        m_pos += size;
        // y is the result of a size request (none made); w, h the new size.
        resizeFramebuffer(w, h);
        return true;
    }

    case kEncCursor: {
        const qsizetype pixels = qsizetype(w) * h * m_bytesPerPixel;
        const qsizetype maskRow = (w + 7) / 8;
        const qsizetype size = 12 + pixels + maskRow * h;
        if (!available(size))
            return false;
        QImage cursor;
        if (w > 0 && h > 0) {
            cursor = QImage(w, h, QImage::Format_ARGB32);
            const auto *src = reinterpret_cast<const uchar *>(m_in.constData() + m_pos + 12);
            const auto *mask = reinterpret_cast<const uchar *>(m_in.constData() + m_pos + 12 + pixels);
            for (int row = 0; row < h; ++row) {
                auto *dst = reinterpret_cast<quint32 *>(cursor.scanLine(row));
                for (int col = 0; col < w; ++col) {
                    const bool opaque = mask[row * maskRow + col / 8] & (0x80 >> (col % 8));
                    dst[col] = opaque ? pixel(src + (row * w + col) * m_bytesPerPixel) : 0;
                }
            }
        }
        m_pos += size;
        emit cursorChanged(cursor, QPoint(x, y));
        return true;
    }

    case kEncQemuKeys:
        m_pos += 12;
        m_scanCodes = true;
        return true;

    default:
        fail(tr("Unsupported update encoding %1").arg(encoding));
        return false;
    }
}

bool VncClient::decodeZrle(const QByteArray &data, const QRect &rect)
{
    // 64-pixel tiles, row by row.
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    const uchar *end = p + data.size();
    const int pixelSize = compactPixelSize();
    const auto pixel = [this, &p, end, pixelSize](quint32 *out) {
        if (end - p < pixelSize)
            return false;
        *out = zrlePixel(p);
        p += pixelSize;
        return true;
    };
    // A run's length: bytes added up while they are 255, plus one.
    const auto runLength = [&p, end](int *out) {
        int length = 1;
        while (true) {
            if (p == end)
                return false;
            const uchar b = *p++;
            length += b;
            if (b != 255)
                break;
        }
        *out = length;
        return true;
    };

    // Each tile is decoded into `tile`, row after row, then copied in whole rows.
    uchar *bits = m_image.bits();
    const qsizetype stride = m_image.bytesPerLine();
    quint32 tile[64 * 64];
    for (int ty = rect.top(); ty <= rect.bottom(); ty += 64) {
        for (int tx = rect.left(); tx <= rect.right(); tx += 64) {
            const int tw = qMin(64, rect.right() + 1 - tx);
            const int th = qMin(64, rect.bottom() + 1 - ty);
            const auto put = [&tile](int index, quint32 colour) { tile[index] = colour; };
            if (p == end)
                return false;
            const int sub = *p++;
            quint32 palette[128];
            const int paletteSize = sub >= 130 ? sub - 128 : sub >= 2 && sub <= 16 ? sub : 0;
            for (int i = 0; i < paletteSize; ++i) {
                if (!pixel(&palette[i]))
                    return false;
            }
            const int count = tw * th;
            if (sub == 0) {   // raw
                for (int i = 0; i < count; ++i) {
                    quint32 c;
                    if (!pixel(&c))
                        return false;
                    put(i, c);
                }
            } else if (sub == 1) {   // one colour
                quint32 c;
                if (!pixel(&c))
                    return false;
                std::fill_n(tile, count, c);
            } else if (sub <= 16) {   // palette indices, packed, rows byte-aligned
                const int bits = paletteSize == 2 ? 1 : paletteSize <= 4 ? 2 : 4;
                const int rowBytes = (tw * bits + 7) / 8;
                if (end - p < qsizetype(rowBytes) * th)
                    return false;
                for (int row = 0; row < th; ++row) {
                    for (int col = 0; col < tw; ++col) {
                        const int bit = col * bits;
                        const int index = (p[bit / 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1);
                        if (index >= paletteSize)
                            return false;
                        put(row * tw + col, palette[index]);
                    }
                    p += rowBytes;
                }
            } else if (sub == 128) {   // runs of a colour
                for (int i = 0; i < count;) {
                    quint32 c;
                    int length;
                    if (!pixel(&c) || !runLength(&length) || length > count - i)
                        return false;
                    for (; length > 0; --length)
                        put(i++, c);
                }
            } else if (sub >= 130) {   // runs of a palette entry, or single entries
                for (int i = 0; i < count;) {
                    if (p == end)
                        return false;
                    const int index = *p & 0x7f;
                    const bool run = *p++ & 0x80;
                    if (index >= paletteSize)
                        return false;
                    int length = 1;
                    if (run && (!runLength(&length) || length > count - i))
                        return false;
                    for (; length > 0; --length)
                        put(i++, palette[index]);
                }
            } else {
                return false;   // 17-127 and 129 are unused
            }
            for (int row = 0; row < th; ++row)
                std::memcpy(bits + (ty + row) * stride + tx * 4, tile + row * tw, tw * 4);
        }
    }
    return true;
}

bool VncClient::readCompactLength(qsizetype at, quint32 *length, int *size) const
{
    // Seven bits a byte, the top one saying another follows; the third has eight.
    quint32 value = 0;
    for (int i = 0; i < 3; ++i) {
        if (!available(at + i + 1))
            return false;
        const quint8 b = u8(at + i);
        value |= quint32(i < 2 ? (b & 0x7f) : b) << (7 * i);
        if (i == 2 || !(b & 0x80)) {
            *length = value;
            *size = i + 1;
            return true;
        }
    }
    return false;
}

bool VncClient::parseTight(const QRect &rect)
{
    // Everything is read before anything is consumed, so that a rectangle
    // that hasn't all come in yet is read again whole next time.
    const int w = rect.width(), h = rect.height();
    qsizetype at = 12;
    if (!available(at + 1))
        return false;
    const quint8 control = u8(at++);
    const int kind = control >> 4;

    const auto resetStreams = [this, control]() {
        for (int i = 0; i < 4; ++i) {
            if (control & (1 << i))
                m_tight[i] = std::make_unique<Inflater>();
        }
    };
    const auto rows = [this, &rect](int row) {
        return reinterpret_cast<quint32 *>(m_image.scanLine(rect.y() + row)) + rect.x();
    };
    const int pixelSize = compactPixelSize();

    if (kind == 8) {   // fill
        if (!available(at + pixelSize))
            return false;
        const quint32 colour = tightPixel(reinterpret_cast<const uchar *>(m_in.constData() + m_pos + at));
        m_pos += at + pixelSize;
        resetStreams();
        for (int row = 0; row < h; ++row)
            std::fill_n(rows(row), w, colour);
        ++m_statsRects[QStringLiteral("tight")];
        return true;
    }

    if (kind == 9) {   // JPEG
        quint32 length;
        int lengthSize;
        if (!readCompactLength(at, &length, &lengthSize) || !available(at + lengthSize + length))
            return false;
        const QByteArray jpeg = QByteArray::fromRawData(m_in.constData() + m_pos + at + lengthSize,
                                                        length);
        QImage image = QImage::fromData(jpeg, "JPEG");
        m_pos += at + lengthSize + length;
        resetStreams();
        if (image.size() != rect.size()) {
            fail(tr("Corrupt JPEG update"));
            return false;
        }
        image.convertTo(QImage::Format_RGB32);
        for (int row = 0; row < h; ++row)
            std::memcpy(rows(row), image.constScanLine(row), w * 4);
        ++m_statsRects[QStringLiteral("tight-jpeg")];
        return true;
    }

    if (kind > 7) {
        fail(tr("Unsupported Tight compression %1").arg(kind));
        return false;
    }

    // Basic: pixels through a filter, zlib-compressed unless few.
    const int stream = kind & 3;
    int filter = 0;   // copy
    if (kind & 4) {
        if (!available(at + 1))
            return false;
        filter = u8(at++);
    }
    quint32 palette[256];
    int colours = 0;
    if (filter == 1) {   // palette
        if (!available(at + 1))
            return false;
        colours = u8(at++) + 1;
        if (!available(at + pixelSize * colours))
            return false;
        for (int i = 0; i < colours; ++i)
            palette[i] = tightPixel(
                reinterpret_cast<const uchar *>(m_in.constData() + m_pos + at + pixelSize * i));
        at += pixelSize * colours;
    } else if (filter > 2) {
        fail(tr("Unsupported Tight filter %1").arg(filter));
        return false;
    }
    const qsizetype rowBytes = filter != 1 ? qsizetype(w) * pixelSize : colours <= 2 ? (w + 7) / 8 : w;
    const qsizetype rawSize = rowBytes * h;

    QByteArray data;
    if (rawSize < 12) {   // too little to be worth compressing
        if (!available(at + rawSize))
            return false;
        data = m_in.mid(m_pos + at, rawSize);
        m_pos += at + rawSize;
        resetStreams();
    } else {
        quint32 length;
        int lengthSize;
        if (!readCompactLength(at, &length, &lengthSize) || !available(at + lengthSize + length))
            return false;
        const qsizetype start = m_pos + at + lengthSize;
        m_pos += at + lengthSize + length;
        resetStreams();
        if (!m_tight[stream]->inflate(m_in.constData() + start, length, &data, rawSize)
            || data.size() < rawSize) {
            fail(tr("Corrupt Tight update"));
            return false;
        }
    }

    const auto *src = reinterpret_cast<const uchar *>(data.constData());
    if (filter == 0) {   // copy
        for (int row = 0; row < h; ++row) {
            quint32 *dst = rows(row);
            for (int col = 0; col < w; ++col, src += pixelSize)
                dst[col] = tightPixel(src);
        }
    } else if (filter == 1) {   // palette indices: a bit each for two colours, else a byte
        for (int row = 0; row < h; ++row, src += rowBytes) {
            quint32 *dst = rows(row);
            for (int col = 0; col < w; ++col) {
                const int index = colours <= 2 ? (src[col / 8] >> (7 - col % 8)) & 1 : src[col];
                if (index >= colours) {
                    fail(tr("Corrupt Tight update"));
                    return false;
                }
                dst[col] = palette[index];
            }
        }
    } else {   // gradient: each channel is the difference from a prediction
        const bool wide = pixelSize == 3;
        const int maximum[3] = {wide ? 255 : 31, wide ? 255 : 63, wide ? 255 : 31};
        std::vector<int> previous(size_t(w) * 3, 0), current(size_t(w) * 3);
        for (int row = 0; row < h; ++row) {
            quint32 *dst = rows(row);
            for (int col = 0; col < w; ++col, src += pixelSize) {
                int difference[3];
                if (wide) {
                    difference[0] = src[0], difference[1] = src[1], difference[2] = src[2];
                } else {
                    const quint16 v = qFromUnaligned<quint16>(src);
                    difference[0] = v >> 11, difference[1] = (v >> 5) & 63, difference[2] = v & 31;
                }
                int *channel = &current[col * 3];
                for (int c = 0; c < 3; ++c) {
                    const int left = col ? current[(col - 1) * 3 + c] : 0;
                    const int above = previous[col * 3 + c];
                    const int aboveLeft = col ? previous[(col - 1) * 3 + c] : 0;
                    const int estimate = qBound(0, left + above - aboveLeft, maximum[c]);
                    channel[c] = (estimate + difference[c]) & maximum[c];
                }
                dst[col] = wide ? 0xff000000u | (channel[0] << 16) | (channel[1] << 8) | channel[2]
                                : fromRgb565(channel[0], channel[1], channel[2]);
            }
            previous.swap(current);
        }
    }
    ++m_statsRects[QStringLiteral("tight")];
    return true;
}

quint32 VncClient::pixel(const uchar *p) const
{
    // In our byte order, as asked for; the spare byte of a 32-bit one is
    // RGB32's alpha: opaque.
    return m_bytesPerPixel == 4 ? qFromUnaligned<quint32>(p) | 0xff000000u
                                : fromRgb565(qFromUnaligned<quint16>(p));
}

quint32 VncClient::zrlePixel(const uchar *p) const
{
    // A 32-bit pixel without its spare byte: 0x00RRGGBB's low three bytes,
    // least significant first.
    return m_bytesPerPixel == 4 ? 0xff000000u | p[0] | (p[1] << 8) | (p[2] << 16) : pixel(p);
}

quint32 VncClient::tightPixel(const uchar *p) const
{
    // Tight's three bytes are red, green, blue, whatever the format's order.
    return m_bytesPerPixel == 4 ? 0xff000000u | (p[0] << 16) | (p[1] << 8) | p[2] : pixel(p);
}

void VncClient::resizeFramebuffer(int width, int height)
{
    // QEMU repeats the size with every full update: only a change counts.
    if (m_image.size() == QSize(width, height))
        return;
    m_image = QImage(width, height, QImage::Format_RGB32);
    m_image.fill(Qt::black);
    m_fullUpdate = true;
    emit resized(m_image.size());
}

void VncClient::setCredentials(const QString &username, const QString &password)
{
    m_username = username;
    m_password = password.isNull() ? QStringLiteral("") : password;
    if (m_state == State::AwaitingCredentials) {
        sendAuthentication();
        onReadyRead();   // anything that came in while waiting
    }
}

void VncClient::sendAuthentication()
{
    if (m_password.isNull()) {
        m_state = State::AwaitingCredentials;
        emit credentialsRequested(m_securityType == kSecArd);
        return;
    }
    if (m_securityType == kSecArd)
        sendArdResponse();
    else
        sendChallengeResponse();
}

void VncClient::sendChallengeResponse()
{
    // The challenge encrypted with the password as the DES key, each of its
    // bytes bit-reversed (an accident of the original implementation that
    // every server keeps).
    quint8 key[8] = {};
    const QByteArray latin1 = m_password.toLatin1().left(8);
    for (int i = 0; i < latin1.size(); ++i) {
        quint8 b = quint8(latin1.at(i)), reversed = 0;
        for (int bit = 0; bit < 8; ++bit)
            reversed |= ((b >> bit) & 1) << (7 - bit);
        key[i] = reversed;
    }
    QByteArray response = m_challenge;
    auto *blocks = reinterpret_cast<quint8 *>(response.data());
    desEncryptBlock(key, blocks);
    desEncryptBlock(key, blocks + 8);
    m_socket->write(response);
    m_state = State::SecurityResult;
}

void VncClient::sendArdResponse()
{
    // Diffie-Hellman with the server's parameters; the shared secret's MD5
    // is the AES key for the name and password, each in 64 bytes, ended by
    // a zero and padded at random.
    const QByteArray privateKey = randomBytes(int(m_ardPrime.size()));
    const QByteArray publicKey = modPow(m_ardGenerator, privateKey, m_ardPrime);
    const QByteArray secret = modPow(m_ardServerKey, privateKey, m_ardPrime);
    if (publicKey.isEmpty() || secret.isEmpty()) {
        fail(tr("The server's login parameters are unusable"));
        return;
    }
    const QByteArray key = QCryptographicHash::hash(secret, QCryptographicHash::Md5);

    QByteArray credentials = randomBytes(128);
    const QByteArray username = m_username.toUtf8().left(63) + '\0';
    const QByteArray password = m_password.toUtf8().left(63) + '\0';
    credentials.replace(0, username.size(), username);
    credentials.replace(64, password.size(), password);
    auto *blocks = reinterpret_cast<quint8 *>(credentials.data());
    for (int i = 0; i < 128; i += 16)
        aes128EncryptBlock(reinterpret_cast<const quint8 *>(key.constData()), blocks + i);

    qCDebug(lcVnc) << "ARD: sending login for" << m_username << "with a"
                   << m_password.size() << "character password";
    m_socket->write(credentials + publicKey);
    m_state = State::SecurityResult;
}

void VncClient::sendClientInit()
{
    QByteArray init;
    put8(init, 1);   // shared: leave other viewers (noVNC) connected
    m_socket->write(init);
    m_state = State::ServerInit;
}

void VncClient::sendSetup()
{
    // SetPixelFormat, in our byte order: 32-bit 0x00RRGGBB, which is
    // QImage's RGB32, or 16-bit RGB565.
    m_bytesPerPixel = m_reducedColour ? 2 : 4;
    const bool wide = m_bytesPerPixel == 4;
    QByteArray out;
    put8(out, 0);
    out.append(3, '\0');
    put8(out, wide ? 32 : 16);   // bits per pixel
    put8(out, wide ? 24 : 16);   // depth
    put8(out, QSysInfo::ByteOrder == QSysInfo::BigEndian);
    put8(out, 1);                // true colour
    put16(out, wide ? 255 : 31);
    put16(out, wide ? 255 : 63);
    put16(out, wide ? 255 : 31);
    put8(out, wide ? 16 : 11);   // red shift
    put8(out, wide ? 8 : 5);     // green shift
    put8(out, 0);                // blue shift
    out.append(3, '\0');

    // Compressed over a network, where bandwidth is what's short. On this
    // machine (a VM, a tunnel) compressing only costs the server time, and
    // raw is quicker.
    const bool local = m_socket->peerAddress().isLoopback();
    QList<qint32> encodings =
        local ? QList<qint32>{kEncCopyRect, kEncRaw, kEncZrle, kEncZlib, kEncTight}
              : QList<qint32>{kEncTight, kEncZrle, kEncCopyRect, kEncZlib, kEncRaw, kEncJpegQuality};
    if (const QString only = qEnvironmentVariable("SUPERTERM_VNC_ENCODINGS"); !only.isEmpty()) {
        const QHash<QString, qint32> names = {
            {QStringLiteral("tight"), kEncTight}, {QStringLiteral("zrle"), kEncZrle},
            {QStringLiteral("copyrect"), kEncCopyRect}, {QStringLiteral("zlib"), kEncZlib},
            {QStringLiteral("raw"), kEncRaw}, {QStringLiteral("jpeg"), kEncJpegQuality},
        };
        encodings.clear();
        for (const QString &name : only.split(QLatin1Char(','), Qt::SkipEmptyParts))
            encodings << names.value(name.trimmed().toLower(), kEncRaw);
    }
    qCDebug(lcVnc) << (local ? "local server" : "remote server") << "- encodings" << encodings;
    encodings << kEncDesktopSize << kEncExtendedDesktopSize << kEncCursor << kEncQemuKeys;
    put8(out, 2);    // SetEncodings
    put8(out, 0);
    put16(out, quint16(encodings.size()));
    for (qint32 encoding : encodings)
        put32(out, quint32(encoding));
    m_socket->write(out);
}

void VncClient::requestUpdate(bool incremental)
{
    QByteArray out;
    put8(out, 3);
    put8(out, incremental);
    put16(out, 0);
    put16(out, 0);
    put16(out, quint16(m_image.width()));
    put16(out, quint16(m_image.height()));
    m_socket->write(out);
}

void VncClient::sendPointer(const QPoint &pos, int buttons)
{
    if (!isConnected())
        return;
    QByteArray out;
    put8(out, 5);
    put8(out, quint8(buttons));
    put16(out, quint16(qBound(0, pos.x(), m_image.width() - 1)));
    put16(out, quint16(qBound(0, pos.y(), m_image.height() - 1)));
    m_socket->write(out);
}

void VncClient::sendKey(bool down, quint32 keysym, quint32 scanCode)
{
    if (!isConnected())
        return;
    QByteArray out;
    if (m_scanCodes && scanCode) {
        // QEMU extended key event. A two-byte code 0xE0nn goes as nn | 0x80.
        const quint32 code = (scanCode >> 8) == 0xe0 ? ((scanCode & 0x7f) | 0x80) : scanCode;
        put8(out, 255);
        put8(out, 0);
        put16(out, down);
        put32(out, keysym);
        put32(out, code);
    } else {
        if (!keysym)
            return;
        put8(out, 4);
        put8(out, down);
        put16(out, 0);
        put32(out, keysym);
    }
    m_socket->write(out);
}

void VncClient::sendClipboard(const QString &text)
{
    if (!isConnected())
        return;
    const QByteArray latin1 = text.toLatin1();
    QByteArray out;
    put8(out, 6);
    out.append(3, '\0');
    put32(out, quint32(latin1.size()));
    out.append(latin1);
    m_socket->write(out);
}
