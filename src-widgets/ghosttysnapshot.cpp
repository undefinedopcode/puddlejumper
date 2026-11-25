/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "ghosttysnapshot.h"

#include <QSet>

namespace GhosttySnapshot {

namespace {

constexpr std::array<quint32, 256> makeCrcTable()
{
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;  // Castagnoli, reflected
        t[i] = c;
    }
    return t;
}
constexpr std::array<quint32, 256> kCrcTable = makeCrcTable();

bool validScalar(quint32 cp)
{
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

QString tagName(quint16 tag)
{
    switch (tag) {
    case TagTerminal: return QStringLiteral("TERMINAL");
    case TagScreen: return QStringLiteral("SCREEN");
    case TagPage: return QStringLiteral("PAGE");
    case TagHistory: return QStringLiteral("HISTORY");
    case TagReady: return QStringLiteral("READY");
    case TagFinish: return QStringLiteral("FINISH");
    case TagContinuation: return QStringLiteral("CONTINUATION");
    case TagKittyImageStorage: return QStringLiteral("KITTY_IMAGE_STORAGE");
    case TagKittyImagePayload: return QStringLiteral("KITTY_IMAGE_PAYLOAD");
    case TagKittyImagePagePlacements: return QStringLiteral("KITTY_IMAGE_PAGE_PLACEMENTS");
    }
    return QStringLiteral("tag %1").arg(tag);
}

bool knownTag(quint16 tag)
{
    return tag >= TagTerminal && tag <= TagKittyImagePagePlacements;
}

quint8 clampEnum(quint8 v, quint8 max, quint8 fallback = 0)
{
    return v <= max ? v : fallback;
}

Rgb rgb(Reader &r)
{
    Rgb c;
    c.r = r.u8();
    c.g = r.u8();
    c.b = r.u8();
    return c;
}

DynamicRgb dynamicRgb(Reader &r)
{
    DynamicRgb d;
    const bool hasDefault = r.u8() != 0;
    const Rgb def = rgb(r);
    const bool hasOverride = r.u8() != 0;
    const Rgb over = rgb(r);
    if (hasDefault)
        d.defaultValue = def;
    if (hasOverride)
        d.override = over;
    return d;
}

Charsets charsets(Reader &r)
{
    const quint16 raw = r.u16();
    Charsets c;
    for (int i = 0; i < 4; ++i)
        c.g[i] = (raw >> (2 * i)) & 3;
    c.gl = (raw >> 8) & 3;
    c.gr = (raw >> 10) & 3;
    c.singleShift = clampEnum((raw >> 12) & 7, 4);
    return c;
}

KittyPlacementKey placementKey(Reader &r)
{
    KittyPlacementKey k;
    k.imageId = r.u32();
    k.external = r.u8() == 1;
    k.id = r.u32();
    return k;
}

} // namespace

quint32 crc32c(QByteArrayView data, quint32 crc)
{
    crc = ~crc;
    for (char ch : data)
        crc = kCrcTable[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int kittyBytesPerPixel(KittyFormat f)
{
    switch (f) {
    case KittyRgb: return 3;
    case KittyRgba: return 4;
    case KittyGrayAlpha: return 2;
    case KittyGray: return 1;
    case KittyPng: return 0;
    }
    return 0;
}

// ---- Reader -----------------------------------------------------------------

void Reader::fail(const QString &why)
{
    if (m_error.isEmpty())
        m_error = why;
    m_pos = m_data.size();
}

#define PJ_READ_LE(T, N)                                                   \
    if (!ok() || remaining() < N) {                                        \
        fail(QStringLiteral("truncated at byte %1").arg(m_pos));           \
        return 0;                                                          \
    }                                                                      \
    T v = 0;                                                               \
    for (int i = 0; i < N; ++i)                                            \
        v = T(v | T(T(quint8(m_data[m_pos + i])) << (8 * i)));             \
    m_pos += N;                                                            \
    return v;

quint8 Reader::u8() { PJ_READ_LE(quint8, 1) }
quint16 Reader::u16() { PJ_READ_LE(quint16, 2) }
quint32 Reader::u32() { PJ_READ_LE(quint32, 4) }
quint64 Reader::u64() { PJ_READ_LE(quint64, 8) }
#undef PJ_READ_LE

QByteArrayView Reader::bytes(quint64 n)
{
    if (!ok() || quint64(remaining()) < n) {
        fail(QStringLiteral("truncated at byte %1 (wanted %2 more)").arg(m_pos).arg(n));
        return {};
    }
    const QByteArrayView v = m_data.sliced(m_pos, qsizetype(n));
    m_pos += qsizetype(n);
    return v;
}

void Reader::expectEnd(const char *what)
{
    if (ok() && !atEnd())
        fail(QStringLiteral("%1 has %2 trailing bytes").arg(QLatin1String(what)).arg(remaining()));
}

// ---- Model helpers ---------------------------------------------------------

int Screen::pageRows() const
{
    int n = 0;
    for (const Page &p : pages)
        n += p.rows;
    return n;
}

const Screen *Snapshot::screen(quint16 key) const
{
    for (const Screen &s : screens)
        if (s.key == key)
            return &s;
    return nullptr;
}

// ---- Pieces -------------------------------------------------------------------

namespace Decode {

void envelope(Reader &r)
{
    const QByteArrayView magic = r.bytes(8);
    if (r.ok() && magic != QByteArrayView(kMagic, 8)) {
        r.fail(QStringLiteral("not a Ghostty snapshot (bad magic)"));
        return;
    }
    const quint16 version = r.u16();
    if (r.ok() && version != kVersion)
        r.fail(QStringLiteral("unsupported snapshot version %1").arg(version));
}

RecordHeader recordHeader(Reader &r)
{
    RecordHeader h;
    h.tag = r.u16();
    h.length = r.u32();
    h.crc = r.u32();
    return h;
}

QByteArrayView record(Reader &r, RecordHeader *header)
{
    const qsizetype start = r.position();
    const RecordHeader h = recordHeader(r);
    if (!r.ok())
        return {};
    if (quint64(h.length) > quint64(r.remaining())) {
        r.fail(QStringLiteral("%1 record at byte %2 is truncated (%3 of %4 payload bytes)")
                   .arg(tagName(h.tag)).arg(start).arg(r.remaining()).arg(h.length));
        return {};
    }
    const QByteArrayView payload = r.bytes(h.length);
    quint8 framing[6];
    for (int i = 0; i < 2; ++i)
        framing[i] = quint8(h.tag >> (8 * i));
    for (int i = 0; i < 4; ++i)
        framing[2 + i] = quint8(h.length >> (8 * i));
    const quint32 crc = crc32c(payload, crc32c(QByteArrayView(framing, 6)));
    if (crc != h.crc) {
        r.fail(QStringLiteral("%1 record at byte %2 fails its CRC32C (0x%3, computed 0x%4)")
                   .arg(tagName(h.tag)).arg(start)
                   .arg(h.crc, 8, 16, QLatin1Char('0')).arg(crc, 8, 16, QLatin1Char('0')));
        return {};
    }
    if (header)
        *header = h;
    return payload;
}

Color color(Reader &r)
{
    Color c;
    const quint8 kind = r.u8();
    const quint8 a = r.u8(), b = r.u8(), d = r.u8();
    switch (kind) {
    case Color::Palette:
        c.kind = Color::Palette;
        c.index = a;
        break;
    case Color::Rgb:
        c.kind = Color::Rgb;
        c.r = a;
        c.g = b;
        c.b = d;
        break;
    default:  // none, or a kind this version doesn't know: no colour
        break;
    }
    return c;
}

Style style(Reader &r)
{
    Style s;
    s.foreground = color(r);
    s.background = color(r);
    s.underlineColor = color(r);
    const quint16 flags = r.u16();
    r.u16();  // reserved
    s.flags = flags & 0xFF;
    const quint8 underline = (flags >> 8) & 7;
    s.underline = underline <= Style::Dashed ? Style::Underline(underline) : Style::NoUnderline;
    return s;
}

Hyperlink hyperlink(Reader &r, bool allowEmpty)
{
    Hyperlink h;
    const quint8 kind = r.u8();
    switch (kind) {
    case Hyperlink::None:
        break;
    case Hyperlink::Implicit:
        h.kind = Hyperlink::Implicit;
        h.implicitId = r.u32();
        h.uri = r.bytes(r.u32()).toByteArray();
        break;
    case Hyperlink::Explicit:
        h.kind = Hyperlink::Explicit;
        h.id = r.bytes(r.u32()).toByteArray();
        h.uri = r.bytes(r.u32()).toByteArray();
        break;
    default:
        // The layout that follows is unknown, so nothing after it can be read.
        r.fail(QStringLiteral("unknown hyperlink kind %1").arg(kind));
        return h;
    }
    // Pages consume entries they can't represent and drop them.
    if (!allowEmpty && h.kind != Hyperlink::None
        && (h.uri.isEmpty() || (h.kind == Hyperlink::Explicit && h.id.isEmpty())))
        r.fail(QStringLiteral("hyperlink with an empty URI or id"));
    return h;
}

void grid(Reader &r, quint16 rows, quint16 columns, Page &page)
{
    page.rowList.clear();
    page.rowList.reserve(qMin<qsizetype>(rows, r.remaining() / 3 + 1));
    for (quint16 y = 0; y < rows && r.ok(); ++y) {
        Row row;
        const quint8 flags = r.u8();
        const quint16 count = r.u16();
        if (!r.ok())
            break;
        if (count > columns) {
            r.fail(QStringLiteral("row %1 has %2 cells in %3 columns").arg(y).arg(count).arg(columns));
            break;
        }
        row.wrap = flags & 1;
        row.wrapContinuation = flags & 2;
        row.prompt = Row::SemanticPrompt(clampEnum((flags >> 2) & 3, 2));
        const int width = 1 << ((flags >> 4) & 3);
        if (quint64(count) * width > quint64(r.remaining())) {
            r.fail(QStringLiteral("row %1 is truncated").arg(y));
            break;
        }
        row.cells.resize(count);
        for (quint16 x = 0; x < count; ++x) {
            quint64 word;
            switch (width) {
            case 1: word = quint64(r.u8()) << 2; break;
            case 2: word = quint64(r.u16()) << 2; break;
            case 4: word = r.u32(); break;
            default: word = r.u64(); break;
            }
            if (word == 0) {
                row.cells[x].word = 0;
                continue;
            }

            // Normalise, as Ghostty's own decoder does.
            quint64 kind = word & 3;
            quint64 content = (word >> 2) & 0xFFFFFF;
            if (kind <= 1) {
                kind = 0;  // a grapheme suffix, if any, comes later
                if (!validScalar(quint32(content)))
                    content = 0xFFFD;
            } else if (kind == 2) {
                content &= 0xFF;
            }
            quint64 styleId = (word >> 26) & 0xFFFF;
            if (styleId && !page.styles.contains(quint16(styleId)))
                styleId = 0;
            quint64 semantic = (word >> 46) & 3;
            if (semantic == 3)
                semantic = 0;
            quint64 link = word >> 48;
            if (link && !page.hyperlinks.contains(quint16(link)))
                link = 0;
            const quint64 wide = (word >> 42) & 3;
            const quint64 prot = (word >> 44) & 1;
            row.cells[x].word = kind | (content << 2) | (styleId << 26) | (wide << 42)
                | (prot << 44) | (quint64(link != 0) << 45) | (semantic << 46) | (link << 48);
        }

        // Wide pairs: a wide cell needs its tail after it, a tail needs its
        // wide cell before it, and a spacer head only ends a wrapped row.
        auto setWidth = [](Cell &c, quint64 w) { c.word = (c.word & ~(quint64(3) << 42)) | (w << 42); };
        const qsizetype n = row.cells.size();
        for (qsizetype x = 0; x < n; ++x) {
            Cell &c = row.cells[x];
            if (x > 0 && row.cells[x - 1].width() == Cell::Wide && c.width() != Cell::SpacerTail)
                setWidth(row.cells[x - 1], Cell::Narrow);
            switch (c.width()) {
            case Cell::Narrow:
                break;
            case Cell::Wide:
                if (x + 1 == n && n == columns)
                    setWidth(c, Cell::Narrow);
                break;
            case Cell::SpacerTail:
                if (x == 0 || row.cells[x - 1].width() != Cell::Wide)
                    setWidth(c, Cell::Narrow);
                break;
            case Cell::SpacerHead:
                if (x + 1 != columns || !row.wrap)
                    setWidth(c, Cell::Narrow);
                break;
            }
        }
        // A wide cell ending the encoded cells has the implicit default cell
        // after it, which is no tail.
        if (n > 0 && n < columns && row.cells[n - 1].width() == Cell::Wide)
            setWidth(row.cells[n - 1], Cell::Narrow);
        page.rowList.append(std::move(row));
    }

    const quint32 entries = r.u32();
    for (quint32 i = 0; i < entries && r.ok(); ++i) {
        const quint16 y = r.u16();
        const quint16 x = r.u16();
        const quint16 n = r.u16();
        if (!r.ok())
            break;
        if (quint64(n) * 4 > quint64(r.remaining())) {
            r.fail(QStringLiteral("grapheme entry %1 is truncated").arg(i));
            break;
        }
        const quint32 key = Page::graphemeKey(x, y);
        const bool target = y < page.rowList.size() && x < columns
            && page.rowList[y].cell(x).content() == Cell::Codepoint
            && page.rowList[y].cell(x).codepoint() != 0
            && !page.graphemes.contains(key);
        QList<char32_t> suffix;
        for (quint16 k = 0; k < n; ++k) {
            const quint32 cp = r.u32();
            if (target && cp != 0 && validScalar(cp))
                suffix.append(char32_t(cp));
        }
        if (target && !suffix.isEmpty())
            page.graphemes.insert(key, suffix);
    }
}

void pageHeader(Reader &r, Page &page, quint16 *styleCount, quint16 *hyperlinkCount)
{
    page.columns = r.u16();
    page.rows = r.u16();
    *styleCount = r.u16();
    *hyperlinkCount = r.u16();
    page.styleCapacity = r.u16();
    page.hyperlinkCapacityBytes = r.u16();
    page.graphemeCapacityBytes = r.u32();
    page.stringCapacityBytes = r.u32();
    if (r.ok() && (page.columns == 0 || page.rows == 0))
        r.fail(QStringLiteral("page of %1x%2").arg(page.columns).arg(page.rows));
}

Page page(Reader &r)
{
    Page p;
    quint16 styleCount = 0, linkCount = 0;
    pageHeader(r, p, &styleCount, &linkCount);
    if (!r.ok())
        return p;
    for (quint16 i = 0; i < styleCount && r.ok(); ++i) {
        const quint16 id = r.u16();
        const Style s = style(r);
        if (id != 0 && r.ok())
            p.styles.insert(id, s);
    }
    for (quint16 i = 0; i < linkCount && r.ok(); ++i) {
        const quint16 id = r.u16();
        const Hyperlink h = hyperlink(r, true);
        const bool usable = h.kind != Hyperlink::None && !h.uri.isEmpty()
            && (h.kind != Hyperlink::Explicit || !h.id.isEmpty());
        if (id != 0 && usable && r.ok())
            p.hyperlinks.insert(id, h);
    }
    grid(r, p.rows, p.columns, p);
    r.expectEnd("PAGE");
    return p;
}

void terminalHeader(Reader &r, TerminalState &t)
{
    t.columns = r.u16();
    t.rows = r.u16();
    t.widthPx = r.u32();
    t.heightPx = r.u32();
    t.scrollTop = r.u16();
    t.scrollBottom = r.u16();
    t.scrollLeft = r.u16();
    t.scrollRight = r.u16();
    t.statusDisplay = clampEnum(r.u8(), 1);
    t.activeScreen = r.u16();
    t.screenCount = r.u16();
    const quint32 previous = r.u32();
    t.previousCodepoint = validScalar(previous) ? std::optional<char32_t>(char32_t(previous)) : std::nullopt;
    t.cursorIsDefault = r.u8() != 0;
    t.cursorDefaultStyle = CursorStyle(clampEnum(r.u8(), 3, CursorBlock));
    t.cursorDefaultBlink = clampEnum(r.u8(), 2);
    t.shellRedraw = clampEnum(r.u8(), 2);
    t.modifyOtherKeys2 = r.u8() != 0;
    t.mouseEvent = clampEnum(r.u8(), 4);
    t.mouseFormat = clampEnum(r.u8(), 4);
    t.mouseShiftCapture = clampEnum(r.u8(), 2);
    t.mouseShape = clampEnum(r.u8(), 33, 8);  // unknown: text, as Ghostty
    t.passwordInput = r.u8() != 0;
    const quint64 modeMask = (quint64(1) << ModeCount) - 1;
    t.currentModes.bits = r.u64() & modeMask;
    t.savedModes.bits = r.u64() & modeMask;
    t.defaultModes.bits = r.u64() & modeMask;
    t.background = dynamicRgb(r);
    t.foreground = dynamicRgb(r);
    t.cursorColor = dynamicRgb(r);
    auto limit = [](quint64 v) { return v == ~quint64(0) ? std::nullopt : std::optional<quint64>(v); };
    t.maxScrollbackBytes = limit(r.u64());
    t.maxScrollbackRows = limit(r.u64());
    if (!r.ok())
        return;

    // What the rest of the snapshot is framed by must be right; the rest is
    // clamped so a bad value can't point outside the screen.
    if (t.columns == 0 || t.rows == 0)
        return r.fail(QStringLiteral("terminal of %1x%2").arg(t.columns).arg(t.rows));
    if (t.screenCount < 1 || t.screenCount > 2)
        return r.fail(QStringLiteral("%1 screens").arg(t.screenCount));
    if (t.activeScreen >= t.screenCount)
        t.activeScreen = 0;  // as Ghostty: an unusable active screen means the primary
    if (t.scrollBottom >= t.rows || t.scrollTop > t.scrollBottom) {
        t.scrollTop = 0;
        t.scrollBottom = t.rows - 1;
    }
    if (t.scrollRight >= t.columns || t.scrollLeft > t.scrollRight) {
        t.scrollLeft = 0;
        t.scrollRight = t.columns - 1;
    }
}

void terminal(Reader &r, TerminalState &t)
{
    terminalHeader(r, t);
    if (!r.ok())
        return;
    const QByteArrayView stops = r.bytes((t.columns + 7) / 8);
    t.tabStops.resize(t.columns);
    for (int c = 0; c < t.columns && r.ok(); ++c)
        t.tabStops[c] = (quint8(stops[c / 8]) >> (c % 8)) & 1;
    for (int i = 0; i < 256; ++i)
        t.originalPalette[i] = rgb(r);
    const QByteArrayView mask = r.bytes(32);
    for (int i = 0; i < 256 && r.ok(); ++i) {
        if ((quint8(mask[i / 8]) >> (i % 8)) & 1)
            t.paletteOverrides[i] = rgb(r);
        else
            t.paletteOverrides[i].reset();
    }
    t.pwd = r.bytes(r.u32()).toByteArray();
    t.title = r.bytes(r.u32()).toByteArray();
    r.expectEnd("TERMINAL");
}

quint16 screenHeader(Reader &r, Screen &s, bool *savedCursorPresent)
{
    s.key = r.u16();
    const quint16 pageCount = r.u16();
    s.historyRows = r.u64();
    s.cursorX = r.u16();
    s.cursorY = r.u16();
    s.cursorStyle = CursorStyle(clampEnum(r.u8(), 3, CursorBlock));
    const quint8 flags = r.u8();
    s.pendingWrap = flags & 1;
    s.cursorProtected = flags & 2;
    s.cursorSemantic = clampEnum((flags >> 2) & 3, 2);
    s.semanticClearEol = flags & 0x10;
    s.pen = style(r);
    s.hyperlinkImplicitCounter = r.u32();
    s.charsets = charsets(r);
    s.protectedMode = clampEnum(r.u8(), 2);
    s.kittyKeyboardIndex = r.u8() & 7;
    for (quint8 &f : s.kittyKeyboardFlags)
        f = r.u8() & 0x1F;
    s.semanticClickKind = clampEnum(r.u8(), 2);
    s.semanticClickValue = r.u8();
    const bool saved = r.u8() != 0;
    if (savedCursorPresent)
        *savedCursorPresent = saved;
    if (r.ok() && s.key > 1)
        r.fail(QStringLiteral("screen key %1").arg(s.key));
    if (r.ok() && pageCount == 0)
        r.fail(QStringLiteral("screen %1 has no pages").arg(s.key));
    return pageCount;
}

SavedCursor savedCursor(Reader &r)
{
    SavedCursor c;
    c.x = r.u16();
    c.y = r.u16();
    c.pen = style(r);
    const quint8 flags = r.u8();
    c.isProtected = flags & 1;
    c.pendingWrap = flags & 2;
    c.origin = flags & 4;
    c.charsets = charsets(r);
    return c;
}

void screen(Reader &r, Screen &s, quint16 *pageCount)
{
    bool saved = false;
    const quint16 pages = screenHeader(r, s, &saved);
    if (saved)
        s.savedCursor = savedCursor(r);
    s.cursorHyperlink = hyperlink(r, false);
    r.expectEnd("SCREEN");
    if (pageCount)
        *pageCount = pages;
}

quint32 history(Reader &r, quint16 *key)
{
    const quint16 k = r.u16();
    const quint32 pages = r.u32();
    r.expectEnd("HISTORY");
    if (key)
        *key = k;
    return pages;
}

KittyPlacement kittyPlacement(Reader &r, bool inStorage)
{
    KittyPlacement p;
    p.key = placementKey(r);
    const quint8 location = r.u8();
    if (location > KittyPlacement::Relative) {
        // Pin and relative fields follow only for their own locations, so
        // reading on is safe; the placement itself is meaningless.
        p.location = KittyPlacement::Virtual;
        p.columns = p.rows = 0;
    } else {
        p.location = KittyPlacement::Location(location);
    }
    if (location == KittyPlacement::Pin) {
        if (inStorage)
            p.page = r.u16();
        p.y = r.u16();
        p.x = r.u16();
    } else if (location == KittyPlacement::Relative) {
        p.parent = placementKey(r);
        p.horizontalOffset = r.s32();
        p.verticalOffset = r.s32();
    }
    p.xOffset = r.u32();
    p.yOffset = r.u32();
    p.sourceX = r.u32();
    p.sourceY = r.u32();
    p.sourceWidth = r.u32();
    p.sourceHeight = r.u32();
    p.columns = r.u32();
    p.rows = r.u32();
    p.z = r.s32();
    if (location > KittyPlacement::Relative)
        p.columns = p.rows = 0;
    return p;
}

static KittyImage kittyImage(Reader &r)
{
    KittyImage im;
    im.id = r.u32();
    im.number = r.u32();
    im.width = r.u32();
    im.height = r.u32();
    im.format = KittyFormat(clampEnum(r.u8(), KittyGray, KittyRgba));
    const quint8 flags = r.u8();
    im.transient = flags & 1;
    im.implicitId = flags & 2;
    im.dataLength = r.u64();
    if (r.u8() != 0) {
        im.animated = true;
        im.animationState = clampEnum(r.u8(), 2);
        im.maxLoops = r.u32();
        im.currentLoop = r.u32();
        im.currentIndex = r.u32();
        im.rootGapMs = r.u32();
        const quint32 frames = r.u32();
        // Twelve bytes a frame: a count the payload can't hold is a lie.
        if (quint64(frames) * 12 > quint64(r.remaining())) {
            r.fail(QStringLiteral("image %1 declares %2 frames").arg(im.id).arg(frames));
            return im;
        }
        im.frames.reserve(frames);
        for (quint32 i = 0; i < frames && r.ok(); ++i) {
            KittyFrame f;
            f.gapMs = r.u32();
            f.dataLength = r.u64();
            im.frames.append(f);
        }
    }
    return im;
}

static KittyLoading kittyLoading(Reader &r)
{
    KittyLoading l;
    l.id = r.u32();
    l.number = r.u32();
    l.width = r.u32();
    l.height = r.u32();
    l.format = r.u8();
    l.compression = r.u8();
    l.flags = r.u8();
    l.quiet = r.u8();
    r.skip(16);                // response id, image number, placement id, frame
    if (r.u8() != 0)
        r.skip(11 * 4 + 2 + 4 * 5);  // the display parameters
    if (r.u8() != 0)
        r.skip(2 + 7 * 4 + 2 + 5 * 4 + 4 + 1 + 4);  // the frame parameters
    l.data = r.bytes(r.u32()).toByteArray();
    return l;
}

KittyStorage kittyStorage(Reader &r, quint16 *key)
{
    KittyStorage s;
    const quint16 k = r.u16();
    s.nextImageId = r.u32();
    s.nextInternalPlacementId = r.u32();
    const quint32 images = r.u32();
    const quint32 placements = r.u32();
    const bool loading = r.u8() != 0;
    // Each image is at least 27 bytes and each placement 46: counts the
    // payload can't hold are lies, not reasons to allocate.
    if (r.ok() && (quint64(images) * 27 + quint64(placements) * 46 > quint64(r.remaining()))) {
        r.fail(QStringLiteral("KITTY_IMAGE_STORAGE declares %1 images and %2 placements").arg(images).arg(placements));
        return s;
    }
    for (quint32 i = 0; i < images && r.ok(); ++i)
        s.images.append(kittyImage(r));
    for (quint32 i = 0; i < placements && r.ok(); ++i)
        s.placements.append(kittyPlacement(r, true));
    if (loading)
        s.loading = kittyLoading(r);
    r.expectEnd("KITTY_IMAGE_STORAGE");
    if (key)
        *key = k;
    return s;
}

} // namespace Decode

// ---- Whole snapshots --------------------------------------------------------

qsizetype scan(QByteArrayView data, quint32 maxRecord)
{
    if (data.size() < kEnvelopeSize)
        return QByteArrayView(kMagic, 8).startsWith(data.first(qMin<qsizetype>(data.size(), 8))) ? 0 : -1;
    Reader r(data);
    Decode::envelope(r);
    if (!r.ok())
        return -1;
    while (true) {
        if (r.remaining() < kRecordHeaderSize)
            return 0;
        const Decode::RecordHeader h = Decode::recordHeader(r);
        if (h.length > maxRecord)
            return -1;
        if (quint64(r.remaining()) < h.length)
            return 0;
        r.skip(h.length);
        if (h.tag == TagFinish)
            return r.position();
    }
}

namespace {

struct RawRecord
{
    quint16 tag;
    QByteArrayView payload;
    qsizetype offset;
    qsizetype end;
};

// Walks the known records in order, as Ghostty's verifier does. After READY,
// KITTY_IMAGE_PAYLOAD records may come before any record; they are collected.
class Walk
{
public:
    Walk(QList<RawRecord> records, QString *error) : m_records(std::move(records)), m_error(error) {}

    bool failed() const { return !m_error->isEmpty(); }
    void fail(const QString &why)
    {
        if (m_error->isEmpty())
            *m_error = why;
    }

    int peek()
    {
        collectPayloads();
        return m_index < m_records.size() ? int(m_records[m_index].tag) : -1;
    }

    const RawRecord *take(quint16 tag)
    {
        const int actual = peek();
        if (actual != tag) {
            fail(actual < 0 ? QStringLiteral("expected %1, found the end").arg(tagName(tag))
                            : QStringLiteral("expected %1 at byte %2, found %3")
                                  .arg(tagName(tag)).arg(m_records[m_index].offset).arg(tagName(quint16(actual))));
            return nullptr;
        }
        return &m_records[m_index++];
    }

    // A KITTY_IMAGE_PAYLOAD in between means the record is absent.
    const RawRecord *takeOptional(quint16 tag)
    {
        if (m_allowPayloads && m_index < m_records.size() && m_records[m_index].tag == TagKittyImagePayload)
            return nullptr;
        return peek() == tag ? take(tag) : nullptr;
    }

    void allowPayloads() { m_allowPayloads = true; }
    const QList<const RawRecord *> &payloads() const { return m_payloads; }
    bool atEnd() { return peek() < 0; }

private:
    void collectPayloads()
    {
        while (m_allowPayloads && m_index < m_records.size() && m_records[m_index].tag == TagKittyImagePayload)
            m_payloads.append(&m_records[m_index++]);
    }

    QList<RawRecord> m_records;
    QString *m_error;
    int m_index = 0;
    bool m_allowPayloads = false;
    QList<const RawRecord *> m_payloads;
};

// Runs one payload decoder; a failure names the record it was in.
template<typename F>
bool decodePayload(const RawRecord &rec, QString *error, F &&f)
{
    Reader r(rec.payload);
    f(r);
    if (!r.ok()) {
        if (error->isEmpty())
            *error = QStringLiteral("%1 record at byte %2: %3").arg(tagName(rec.tag)).arg(rec.offset).arg(r.error());
        return false;
    }
    return true;
}

bool decodePages(Walk &walk, quint32 count, bool history, QList<Page> &out, quint16 key, QString *error)
{
    for (quint32 i = 0; i < count; ++i) {
        const RawRecord *rec = walk.take(TagPage);
        if (!rec)
            return false;
        Page page;
        if (!decodePayload(*rec, error, [&](Reader &r) { page = Decode::page(r); }))
            return false;
        if (history) {
            if (const RawRecord *pl = walk.takeOptional(TagKittyImagePagePlacements)) {
                quint16 plKey = 0;
                QList<KittyPlacement> placements;
                const bool ok = decodePayload(*pl, error, [&](Reader &r) {
                    plKey = r.u16();
                    const quint32 n = r.u32();
                    if (r.ok() && quint64(n) * 46 > quint64(r.remaining()))
                        return r.fail(QStringLiteral("declares %1 placements").arg(n));
                    for (quint32 k = 0; k < n && r.ok(); ++k)
                        placements.append(Decode::kittyPlacement(r, false));
                    r.expectEnd("KITTY_IMAGE_PAGE_PLACEMENTS");
                });
                if (!ok)
                    return false;
                if (plKey != key) {
                    *error = QStringLiteral("KITTY_IMAGE_PAGE_PLACEMENTS for screen %1 in screen %2's history").arg(plKey).arg(key);
                    return false;
                }
                // Images in history are pinned; anything else, or a pin off
                // the page, is dropped.
                for (const KittyPlacement &p : placements)
                    if (p.location == KittyPlacement::Relative
                        || (p.location == KittyPlacement::Pin && p.y < page.rows && p.x < page.columns))
                        page.kittyPlacements.append(p);
            }
        }
        out.append(std::move(page));
    }
    return true;
}

} // namespace

Result decode(QByteArrayView data)
{
    Result result;
    Reader r(data);
    Decode::envelope(r);
    if (!r.ok()) {
        result.error = r.error();
        return result;
    }

    // Frame every record and check its CRC, known or not.
    QList<RawRecord> records;
    qsizetype end = 0;
    while (true) {
        const qsizetype offset = r.position();
        if (r.atEnd()) {
            result.error = QStringLiteral("no FINISH record");
            return result;
        }
        Decode::RecordHeader h;
        const QByteArrayView payload = Decode::record(r, &h);
        if (!r.ok()) {
            result.error = r.error();
            return result;
        }
        if (knownTag(h.tag))
            records.append({h.tag, payload, offset, r.position()});
        if (h.tag == TagFinish) {
            end = r.position();
            break;
        }
    }

    Snapshot snap;
    snap.length = end;
    QString error;
    Walk walk(std::move(records), &error);
    auto failed = [&](const QString &why = QString()) {
        if (!why.isEmpty() && error.isEmpty())
            error = why;
        result.error = error;
        return result;
    };

    const RawRecord *rec = walk.take(TagTerminal);
    if (!rec || !decodePayload(*rec, &error, [&](Reader &rr) { Decode::terminal(rr, snap.terminal); }))
        return failed();
    const TerminalState &t = snap.terminal;

    for (int i = 0; i < t.screenCount; ++i) {
        rec = walk.take(TagScreen);
        if (!rec)
            return failed();
        Screen s;
        quint16 pageCount = 0;
        if (!decodePayload(*rec, &error, [&](Reader &rr) { Decode::screen(rr, s, &pageCount); }))
            return failed();
        if (snap.screen(s.key))
            return failed(QStringLiteral("two SCREEN records for screen %1").arg(s.key));
        if (s.key >= t.screenCount)
            return failed(QStringLiteral("SCREEN %1 in a terminal of %2 screens").arg(s.key).arg(t.screenCount));
        if (!decodePages(walk, pageCount, false, s.pages, s.key, &error))
            return failed();
        if (s.pageRows() < t.rows)
            return failed(QStringLiteral("screen %1's pages have %2 rows for a %3-row terminal").arg(s.key).arg(s.pageRows()).arg(t.rows));
        if (const RawRecord *st = walk.takeOptional(TagKittyImageStorage)) {
            quint16 key = 0;
            KittyStorage storage;
            if (!decodePayload(*st, &error, [&](Reader &rr) { storage = Decode::kittyStorage(rr, &key); }))
                return failed();
            if (key != s.key)
                return failed(QStringLiteral("KITTY_IMAGE_STORAGE for screen %1 after SCREEN %2").arg(key).arg(s.key));
            // Drop placements pinned outside their screen's pages.
            QList<KittyPlacement> kept;
            for (const KittyPlacement &p : std::as_const(storage.placements)) {
                if (p.location == KittyPlacement::Pin
                    && (p.page >= s.pages.size() || p.y >= s.pages[p.page].rows || p.x >= s.pages[p.page].columns))
                    continue;
                kept.append(p);
            }
            storage.placements = kept;
            s.kitty = std::move(storage);
        }
        // Lenient about the cursor: clamp it onto the screen.
        s.cursorX = qMin<quint16>(s.cursorX, t.columns - 1);
        s.cursorY = qMin<quint16>(s.cursorY, t.rows - 1);
        if (s.pendingWrap && s.cursorX != t.columns - 1)
            s.pendingWrap = false;
        snap.screens.append(std::move(s));
    }

    rec = walk.take(TagContinuation);
    if (!rec)
        return failed();
    snap.continuation = rec->payload.toByteArray();
    rec = walk.take(TagReady);
    if (!rec)
        return failed();
    if (!rec->payload.isEmpty())
        return failed(QStringLiteral("READY has a payload"));
    snap.readyOffset = rec->end;
    walk.allowPayloads();

    QSet<quint16> histories;
    for (int i = 0; i < t.screenCount; ++i) {
        rec = walk.take(TagHistory);
        if (!rec)
            return failed();
        quint16 key = 0;
        quint32 pageCount = 0;
        if (!decodePayload(*rec, &error, [&](Reader &rr) { pageCount = Decode::history(rr, &key); }))
            return failed();
        Screen *s = nullptr;
        for (Screen &sc : snap.screens)
            if (sc.key == key)
                s = &sc;
        if (!s || histories.contains(key))
            return failed(QStringLiteral("HISTORY for screen %1 doesn't match a SCREEN").arg(key));
        histories.insert(key);
        if (!decodePages(walk, pageCount, true, s->history, key, &error))
            return failed();
    }
    rec = walk.take(TagFinish);
    if (!rec)
        return failed();
    if (!rec->payload.isEmpty())
        return failed(QStringLiteral("FINISH has a payload"));

    // Pixels. A payload for no image, or of the wrong length, is dropped:
    // an image without pixels is better than no terminal.
    for (const RawRecord *p : walk.payloads()) {
        Reader pr(p->payload);
        const quint16 key = pr.u16();
        const quint32 imageId = pr.u32();
        const quint32 frame = pr.u32();
        const quint64 len = pr.u64();
        const QByteArrayView bytes = pr.bytes(len);
        pr.expectEnd("KITTY_IMAGE_PAYLOAD");
        if (!pr.ok())
            return failed(QStringLiteral("KITTY_IMAGE_PAYLOAD at byte %1: %2").arg(p->offset).arg(pr.error()));
        Screen *s = nullptr;
        for (Screen &sc : snap.screens)
            if (sc.key == key)
                s = &sc;
        if (!s || !s->kitty || frame == 0)
            continue;
        for (KittyImage &im : s->kitty->images) {
            if (im.id != imageId)
                continue;
            if (frame == 1 && len == im.dataLength)
                im.data = bytes.toByteArray();
            else if (frame >= 2 && frame - 2 < quint32(im.frames.size()) && len == im.frames[frame - 2].dataLength)
                im.frames[frame - 2].data = bytes.toByteArray();
            break;
        }
    }

    result.snapshot = std::move(snap);
    return result;
}

} // namespace GhosttySnapshot
