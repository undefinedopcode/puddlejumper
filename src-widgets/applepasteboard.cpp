/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "applepasteboard.h"

#include <QStringDecoder>
#include <QtEndian>

#include <zlib.h>

namespace ApplePasteboard {

namespace {

const QByteArray kUtf8Text = QByteArrayLiteral("public.utf8-plain-text");
const QByteArray kUtf16Text = QByteArrayLiteral("public.utf16-plain-text");
constexpr quint32 kMaxName = 4096;   // a flavor's name; more is not an archive

void put32(QByteArray &out, quint32 v)
{
    const quint32 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char *>(&be), 4);
}

} // namespace

QByteArray packText(const QString &text, quint32 *uncompressedSize)
{
    const QByteArray data = text.toUtf8();
    QByteArray archive;
    put32(archive, 1);   // one flavor
    put32(archive, quint32(kUtf8Text.size()));
    archive.append(kUtf8Text);
    put32(archive, 0);   // reserved
    put32(archive, 0);   // no tags
    put32(archive, quint32(data.size()));
    archive.append(data);
    *uncompressedSize = quint32(archive.size());

    // As the Mac's own: level 9, and flushed, not finished.
    z_stream stream{};
    if (deflateInit(&stream, 9) != Z_OK)
        return {};
    QByteArray out(qsizetype(deflateBound(&stream, uLong(archive.size()))) + 16, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(archive.data());
    stream.avail_in = uInt(archive.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = uInt(out.size());
    const int result = deflate(&stream, Z_SYNC_FLUSH);
    const bool ok = result == Z_OK && stream.avail_in == 0 && stream.avail_out > 0;
    out.resize(out.size() - stream.avail_out);
    deflateEnd(&stream);
    return ok ? out : QByteArray();
}

struct Reader::Stream
{
    z_stream z{};
    bool ok = false;
    Stream() { ok = inflateInit(&z) == Z_OK; }
    ~Stream()
    {
        if (ok)
            inflateEnd(&z);
    }
};

Reader::Reader(qsizetype maxText)
    : m_stream(std::make_unique<Stream>()), m_maxText(maxText)
{
}

Reader::~Reader() = default;

QString Reader::text() const
{
    if (m_hasUtf8)
        return QString::fromUtf8(m_utf8);
    if (m_hasUtf16) {
        // Big-endian as the Mac packs it, unless a mark says otherwise.
        QStringDecoder decode(QStringDecoder::Utf16BE);
        if (m_utf16.startsWith("\xff\xfe") || m_utf16.startsWith("\xfe\xff"))
            decode = QStringDecoder(QStringDecoder::Utf16);
        return decode(m_utf16);
    }
    return QString();
}

void Reader::expect(Field field, quint32 size, bool keep)
{
    m_field = field;
    m_want = size;
    m_keep = keep;
    m_kept.clear();
}

/* The field just completed: act on it and say which comes next. */
bool Reader::next()
{
    const quint32 value = m_kept.size() == 4 ? qFromBigEndian<quint32>(m_kept.constData()) : 0;
    auto nextFlavor = [this]() {
        if (m_flavorsLeft > 0) {
            --m_flavorsLeft;
            expect(Field::NameLength, 4, true);
        } else {
            expect(Field::FlavorCount, 4, true);   // the next item, if any
        }
    };
    auto nextTag = [this]() {
        if (m_tagsLeft > 0) {
            --m_tagsLeft;
            expect(Field::TagKeyLength, 4, true);
        } else {
            expect(Field::DataLength, 4, true);
        }
    };

    switch (m_field) {
    case Field::FlavorCount:
        m_flavorsLeft = value;
        nextFlavor();
        return true;
    case Field::NameLength:
        if (value > kMaxName)
            return false;
        expect(Field::Name, value, true);
        return true;
    case Field::Name:
        m_kind = m_kept == kUtf8Text ? Kind::Utf8 : m_kept == kUtf16Text ? Kind::Utf16 : Kind::Other;
        if (m_kind != Kind::Other)
            m_listsText = true;
        expect(Field::Reserved, 4, false);
        return true;
    case Field::Reserved:
        expect(Field::TagCount, 4, true);
        return true;
    case Field::TagCount:
        m_tagsLeft = value;
        nextTag();
        return true;
    case Field::TagKeyLength:
        expect(Field::TagKey, value, false);
        return true;
    case Field::TagKey:
        expect(Field::TagValueLength, 4, true);
        return true;
    case Field::TagValueLength:
        expect(Field::TagValue, value, false);
        return true;
    case Field::TagValue:
        nextTag();
        return true;
    case Field::DataLength: {
        // Only the first of each kind, and only what is not a promise.
        const bool wanted = value > 0 && qsizetype(value) <= m_maxText
                            && ((m_kind == Kind::Utf8 && !m_hasUtf8)
                                || (m_kind == Kind::Utf16 && !m_hasUtf16));
        if (!wanted)
            m_kind = Kind::Other;
        expect(Field::Data, value, wanted);
        return true;
    }
    case Field::Data:
        if (m_kind == Kind::Utf8) {
            m_utf8 = m_kept;
            m_hasUtf8 = true;
            m_done = true;
        } else if (m_kind == Kind::Utf16) {
            m_utf16 = m_kept;
            m_hasUtf16 = true;
        }
        nextFlavor();
        return true;
    }
    return false;
}

bool Reader::feed(const char *data, qsizetype size)
{
    if (m_broken || !m_stream->ok)
        return false;
    if (m_done)
        return true;
    z_stream &z = m_stream->z;
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
    z.avail_in = uInt(size);
    char buffer[64 * 1024];
    while (!m_done) {
        z.next_out = reinterpret_cast<Bytef *>(buffer);
        z.avail_out = sizeof buffer;
        const int result = inflate(&z, Z_SYNC_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
            m_broken = true;
            return false;
        }
        qsizetype have = qsizetype(sizeof buffer) - z.avail_out;
        const char *at = buffer;
        while (have > 0 && !m_done) {
            const qsizetype take = qMin<qsizetype>(m_want, have);
            if (m_keep)
                m_kept.append(at, take);
            at += take;
            have -= take;
            m_want -= quint32(take);
            while (m_want == 0 && !m_done) {
                if (!next()) {
                    m_broken = true;
                    return false;
                }
            }
        }
        if (z.avail_out > 0 || result == Z_STREAM_END)
            break;   // all of this input is through
    }
    return true;
}

} // namespace ApplePasteboard
