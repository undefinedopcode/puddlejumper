/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <memory>

/*
 * A Mac's pasteboard, as Screen Sharing sends it over RFB 003.889 (see
 * docs/apple-rfb-889.md): a zlib stream holding a run of items, each a count
 * of flavors, and each flavor a counted name ("public.utf8-plain-text"), a
 * reserved word, counted key and value tags, and counted data. Every count
 * is a big-endian u32. A flavor with no data is a promise of it.
 *
 * The format follows remotex's description of it (docs/apple-vnc-889.md,
 * MIT, github.com/andrewtheguy/remotex).
 */
namespace ApplePasteboard {

/* Text as the one flavor of one item, deflated: the body of a message 0x1f. */
QByteArray packText(const QString &text, quint32 *uncompressedSize);

/*
 * Reads an archive as it arrives, keeping only its text: a copied word can
 * come with megabytes of other flavors, which are counted past.
 */
class Reader
{
public:
    /* Text longer than `maxText` bytes is not kept. */
    explicit Reader(qsizetype maxText);
    ~Reader();

    /* More of the compressed archive; false once it is found corrupt. */
    bool feed(const char *data, qsizetype size);

    /* A text flavor was listed, with data or as a promise. */
    bool listsText() const { return m_listsText; }
    /* The text, once read; null if there was none (or too much). */
    QString text() const;

private:
    enum class Field {
        FlavorCount, NameLength, Name, Reserved, TagCount, TagKeyLength, TagKey,
        TagValueLength, TagValue, DataLength, Data,
    };
    enum class Kind { Other, Utf8, Utf16 };

    void expect(Field field, quint32 size, bool keep);
    bool next();

    struct Stream;
    std::unique_ptr<Stream> m_stream;
    qsizetype m_maxText;
    Field m_field = Field::FlavorCount;
    quint32 m_want = 4;          // bytes the field still needs
    bool m_keep = true;          // whether they go to m_kept
    QByteArray m_kept;
    quint32 m_flavorsLeft = 0;
    quint32 m_tagsLeft = 0;
    Kind m_kind = Kind::Other;   // of the flavor being read
    bool m_listsText = false;
    bool m_done = false;         // the UTF-8 text is in: nothing more is wanted
    bool m_broken = false;
    QByteArray m_utf8;
    QByteArray m_utf16;
    bool m_hasUtf8 = false;
    bool m_hasUtf16 = false;
};

} // namespace ApplePasteboard
