/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "vncclient.h"

#include "des.h"

#include <QSysInfo>
#include <QTcpSocket>
#include <QtEndian>

#include <cstring>

namespace {

// Encodings (and pseudo-encodings) we ask for, in order of preference.
constexpr qint32 kEncRaw = 0;
constexpr qint32 kEncCopyRect = 1;
constexpr qint32 kEncDesktopSize = -223;
constexpr qint32 kEncCursor = -239;
constexpr qint32 kEncQemuKeys = -258;
constexpr qint32 kEncExtendedDesktopSize = -308;

constexpr quint32 kMaxText = 16 * 1024 * 1024;   // cut text and reasons

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

VncClient::VncClient(QObject *parent)
    : QObject(parent), m_socket(new QTcpSocket(this))
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
    m_authenticating = false;
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
    m_in.append(m_socket->readAll());
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
        bool none = false, password = false;
        for (int i = 0; i < count; ++i) {
            none |= u8(1 + i) == 1;
            password |= u8(1 + i) == 2;
        }
        m_pos += 1 + count;
        if (!none && !password) {
            fail(tr("The server offers no supported security type"));
            return false;
        }
        QByteArray choice;
        put8(choice, none ? 1 : 2);
        m_socket->write(choice);
        if (!none) {
            m_authenticating = true;
            m_state = State::Challenge;
        } else if (m_minor >= 8) {
            m_state = State::SecurityResult;
        } else {
            sendClientInit();   // 3.7 only sends a result for types that authenticate
        }
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
        if (type == 2) {
            m_authenticating = true;
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
        if (m_password.isNull()) {
            m_state = State::AwaitingPassword;
            emit passwordRequested();
            return false;
        }
        sendChallengeResponse();
        return true;

    case State::AwaitingPassword:
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
            m_authFailed = m_authenticating;
            if (m_authFailed)
                m_password = QString();   // never sent again
            if (reason.isEmpty())
                reason = m_authenticating ? tr("Wrong password") : tr("The server refused the connection");
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
            if (m_rectsLeft == 0) {
                m_state = State::Normal;
                requestUpdate(true);
            }
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
            requestUpdate(!m_fullUpdate);
            m_fullUpdate = false;
        }
        return true;
    }
    return false;
}

bool VncClient::parseRect()
{
    if (!available(12))
        return false;
    const int x = u16(0), y = u16(2), w = u16(4), h = u16(6);
    const qint32 encoding = qint32(u32(8));
    const QRect rect(x, y, w, h);
    const bool inside = m_image.rect().contains(rect) || rect.isEmpty();

    switch (encoding) {
    case kEncRaw: {
        const qsizetype size = 12 + qsizetype(w) * h * 4;
        if (!available(size))
            return false;
        if (!inside) {
            fail(tr("Update outside the screen"));
            return false;
        }
        // The buffer has no alignment to speak of, hence qFromUnaligned.
        const char *src = m_in.constData() + m_pos + 12;
        for (int row = 0; row < h; ++row) {
            auto *dst = reinterpret_cast<quint32 *>(m_image.scanLine(y + row)) + x;
            // The pixel's spare byte is the alpha in QImage's RGB32: opaque.
            for (int col = 0; col < w; ++col, src += 4)
                dst[col] = qFromUnaligned<quint32>(src) | 0xff000000u;
        }
        m_pos += size;
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
        const qsizetype pixels = qsizetype(w) * h * 4;
        const qsizetype maskRow = (w + 7) / 8;
        const qsizetype size = 12 + pixels + maskRow * h;
        if (!available(size))
            return false;
        QImage cursor;
        if (w > 0 && h > 0) {
            cursor = QImage(w, h, QImage::Format_ARGB32);
            const char *src = m_in.constData() + m_pos + 12;
            const auto *mask = reinterpret_cast<const uchar *>(m_in.constData() + m_pos + 12 + pixels);
            for (int row = 0; row < h; ++row) {
                auto *dst = reinterpret_cast<quint32 *>(cursor.scanLine(row));
                for (int col = 0; col < w; ++col) {
                    const bool opaque = mask[row * maskRow + col / 8] & (0x80 >> (col % 8));
                    dst[col] = opaque ? (qFromUnaligned<quint32>(src + (row * w + col) * 4) | 0xff000000u)
                                      : 0;
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

void VncClient::setPassword(const QString &password)
{
    m_password = password.isNull() ? QStringLiteral("") : password;
    if (m_state == State::AwaitingPassword) {
        sendChallengeResponse();
        onReadyRead();   // anything that came in while waiting
    }
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

void VncClient::sendClientInit()
{
    QByteArray init;
    put8(init, 1);   // shared: leave other viewers (noVNC) connected
    m_socket->write(init);
    m_state = State::ServerInit;
}

void VncClient::sendSetup()
{
    // SetPixelFormat: 32-bit 0x00RRGGBB in our byte order, which is QImage's RGB32.
    QByteArray out;
    put8(out, 0);
    out.append(3, '\0');
    put8(out, 32);   // bits per pixel
    put8(out, 24);   // depth
    put8(out, QSysInfo::ByteOrder == QSysInfo::BigEndian);
    put8(out, 1);    // true colour
    put16(out, 255);
    put16(out, 255);
    put16(out, 255);
    put8(out, 16);   // red shift
    put8(out, 8);    // green shift
    put8(out, 0);    // blue shift
    out.append(3, '\0');

    const qint32 encodings[] = {
        kEncCopyRect, kEncRaw, kEncDesktopSize, kEncExtendedDesktopSize,
        kEncCursor, kEncQemuKeys,
    };
    put8(out, 2);    // SetEncodings
    put8(out, 0);
    put16(out, quint16(std::size(encodings)));
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
