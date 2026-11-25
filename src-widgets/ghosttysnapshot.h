/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QString>

#include <array>
#include <optional>

/*
 * Ghostty's terminal snapshot format, version 1: a terminal's whole state
 * (modes, palette, both screens, scrollback, Kitty images, a half-parsed
 * escape sequence) as one blob. Rex sends one at the start of every terminal
 * stream. Ghostty documents it in src/terminal/snapshot/ (snapshot.ksy is the
 * byte-level schema; a copy is in src-widgets/dev/ghostty-snapshot/).
 *
 *   envelope   "GHOSTSNP" u16 version=1
 *   records    u16 tag, u32 length, u32 CRC32C(tag, length, payload), payload
 *
 *   TERMINAL, then per screen: SCREEN, its PAGEs (oldest first), optional
 *   KITTY_IMAGE_STORAGE; CONTINUATION; READY; then per screen: HISTORY and its
 *   PAGEs (newest first), each optionally followed by its
 *   KITTY_IMAGE_PAGE_PLACEMENTS; FINISH. KITTY_IMAGE_PAYLOAD records may come
 *   anywhere after READY except between a history PAGE and its placements.
 *   Records with unknown tags are skipped anywhere, after their CRC is
 *   checked: that is how the format grows within version 1.
 *
 * This decodes into plain data and goes no further; mapping it onto a
 * terminal emulator is someone else's job. As Ghostty's decoder does, it is
 * strict about structure (lengths, counts, order, CRCs: anything that would
 * misframe what follows) and lenient about values: unknown enum values,
 * invalid codepoints, dangling style or hyperlink ids and inconsistent wide
 * cells degrade to defaults instead of failing.
 *
 * Memory stays proportional to the input: rows keep only their encoded cells
 * (the rest of a row is default cells), so a page that declares 65535
 * columns costs nothing until it carries them.
 */
namespace GhosttySnapshot {

enum Tag : quint16 {
    TagTerminal = 1,
    TagScreen = 2,
    TagPage = 3,
    TagHistory = 4,
    TagReady = 5,
    TagFinish = 6,
    TagContinuation = 7,
    TagKittyImageStorage = 8,
    TagKittyImagePayload = 9,
    TagKittyImagePagePlacements = 10,
};

inline constexpr char kMagic[] = "GHOSTSNP";
inline constexpr quint16 kVersion = 1;
inline constexpr int kEnvelopeSize = 10;
inline constexpr int kRecordHeaderSize = 10;

quint32 crc32c(QByteArrayView data, quint32 crc = 0);

// ---- Little-endian reading with a sticky failure. -----------------------

class Reader
{
public:
    explicit Reader(QByteArrayView data) : m_data(data) {}

    bool ok() const { return m_error.isEmpty(); }
    const QString &error() const { return m_error; }
    qsizetype position() const { return m_pos; }
    qsizetype remaining() const { return m_data.size() - m_pos; }
    bool atEnd() const { return m_pos == m_data.size(); }

    // The first failure wins; later reads return zeros.
    void fail(const QString &why);

    quint8 u8();
    quint16 u16();
    quint32 u32();
    quint64 u64();
    qint32 s32() { return qint32(u32()); }
    QByteArrayView bytes(quint64 n);
    void skip(quint64 n) { bytes(n); }

    // Requires the reader to be exhausted, naming what was being read.
    void expectEnd(const char *what);

private:
    QByteArrayView m_data;
    qsizetype m_pos = 0;
    QString m_error;
};

// ---- Styles, colours, hyperlinks. -----------------------------------------

struct Color
{
    enum Kind : quint8 { None = 0, Palette = 1, Rgb = 2 };
    Kind kind = None;
    quint8 index = 0;           // Palette
    quint8 r = 0, g = 0, b = 0; // Rgb

    bool operator==(const Color &o) const { return kind == o.kind && index == o.index && r == o.r && g == o.g && b == o.b; }
};

struct Style
{
    enum Flag : quint16 {
        Bold = 1 << 0,
        Italic = 1 << 1,
        Faint = 1 << 2,
        Blink = 1 << 3,
        Inverse = 1 << 4,
        Invisible = 1 << 5,
        Strikethrough = 1 << 6,
        Overline = 1 << 7,
    };
    enum Underline : quint8 { NoUnderline, Single, Double, Curly, Dotted, Dashed };

    Color foreground, background, underlineColor;
    quint16 flags = 0;   // Flag bits only
    Underline underline = NoUnderline;

    bool has(Flag f) const { return flags & f; }
    bool operator==(const Style &o) const
    {
        return foreground == o.foreground && background == o.background && underlineColor == o.underlineColor
            && flags == o.flags && underline == o.underline;
    }
};

struct Hyperlink
{
    enum Kind : quint8 { None = 0, Implicit = 1, Explicit = 2 };
    Kind kind = None;
    quint32 implicitId = 0;  // Implicit
    QByteArray id;           // Explicit
    QByteArray uri;
};

// ---- Pages: rows of cells. --------------------------------------------------

// One 64-bit cell word, already normalised by the decoder.
struct Cell
{
    enum Content : quint8 { Codepoint = 0, PaletteBackground = 2, RgbBackground = 3 };
    enum Width : quint8 { Narrow = 0, Wide = 1, SpacerTail = 2, SpacerHead = 3 };
    enum Semantic : quint8 { Output = 0, Input = 1, Prompt = 2 };

    quint64 word = 0;

    Content content() const { return Content(word & 3); }
    // Codepoint cells: the character, 0 for an empty cell.
    char32_t codepoint() const { return content() == Codepoint ? char32_t((word >> 2) & 0xFFFFFF) : 0; }
    // PaletteBackground: the index. RgbBackground: 0xRRGGBB.
    quint8 paletteIndex() const { return quint8(word >> 2); }
    quint32 rgb() const
    {
        const quint32 c = (word >> 2) & 0xFFFFFF;
        return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
    }
    quint16 styleId() const { return quint16(word >> 26); }
    Width width() const { return Width((word >> 42) & 3); }
    bool isProtected() const { return (word >> 44) & 1; }
    Semantic semantic() const { return Semantic((word >> 46) & 3); }
    quint16 hyperlinkId() const { return quint16(word >> 48); }
};

struct Row
{
    enum SemanticPrompt : quint8 { NoPrompt = 0, PromptStart = 1, PromptContinuation = 2 };

    bool wrap = false;
    bool wrapContinuation = false;
    SemanticPrompt prompt = NoPrompt;
    QList<Cell> cells;  // the encoded cells; every later column is a default cell

    Cell cell(int x) const { return x < cells.size() ? cells[x] : Cell(); }
};

struct KittyPlacement;

struct Page
{
    quint16 columns = 0;
    quint16 rows = 0;
    // Allocation hints from the source page; safe to ignore.
    quint16 styleCapacity = 0;
    quint16 hyperlinkCapacityBytes = 0;
    quint32 graphemeCapacityBytes = 0;
    quint32 stringCapacityBytes = 0;

    QHash<quint16, Style> styles;          // by the ids cells use; 0 is the default style
    QHash<quint16, Hyperlink> hyperlinks;  // by the ids cells use; 0 is none
    QList<Row> rowList;
    // Codepoints after the cell's own, keyed by grapheme(x, y).
    QHash<quint32, QList<char32_t>> graphemes;

    // History pages only: the Kitty placements pinned to this page.
    QList<KittyPlacement> kittyPlacements;

    static quint32 graphemeKey(int x, int y) { return (quint32(y) << 16) | quint32(x); }
    QList<char32_t> grapheme(int x, int y) const { return graphemes.value(graphemeKey(x, y)); }
};

// ---- Terminal-wide state. ---------------------------------------------------

struct Rgb
{
    quint8 r = 0, g = 0, b = 0;
    bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
};

// A colour with an optional default (set by configuration) and an optional
// override (set by the program, OSC 10/11/12).
struct DynamicRgb
{
    std::optional<Rgb> defaultValue;
    std::optional<Rgb> override;
};

// The packed mode registry (43 bits). Bit numbers are the wire's.
enum Mode : quint8 {
    ModeDisableKeyboard, ModeInsert, ModeSendReceive, ModeLinefeed,
    ModeCursorKeys, ModeColumn132, ModeSlowScroll, ModeReverseColors,
    ModeOrigin, ModeWraparound, ModeAutorepeat, ModeMouseX10,
    ModeCursorBlinking, ModeCursorVisible, ModeEnableMode3, ModeReverseWrap,
    ModeAltScreenLegacy, ModeKeypadKeys, ModeBackarrowKey, ModeLeftRightMargin,
    ModeMouseNormal, ModeMouseButton, ModeMouseAny, ModeFocusEvent,
    ModeMouseUtf8, ModeMouseSgr, ModeMouseAlternateScroll, ModeMouseUrxvt,
    ModeMouseSgrPixels, ModeIgnoreKeypadWithNumlock, ModeAltEscPrefix, ModeAltSendsEscape,
    ModeReverseWrapExtended, ModeAltScreen, ModeSaveCursor, ModeAltScreenSaveCursorClearEnter,
    ModeBracketedPaste, ModeSynchronizedOutput, ModeGraphemeCluster, ModeReportColorScheme,
    ModeReportVisibility, ModeInBandSizeReports, ModeKittyPasteEvents,
    ModeCount
};

struct Modes
{
    quint64 bits = 0;
    bool has(Mode m) const { return (bits >> m) & 1; }
};

enum CursorStyle : quint8 { CursorBar = 0, CursorBlock = 1, CursorUnderline = 2, CursorBlockHollow = 3 };

struct TerminalState
{
    quint16 columns = 0;
    quint16 rows = 0;
    quint32 widthPx = 0;
    quint32 heightPx = 0;
    // Scrolling region, inclusive.
    quint16 scrollTop = 0, scrollBottom = 0, scrollLeft = 0, scrollRight = 0;
    quint8 statusDisplay = 0;        // 0 main, 1 status line
    quint16 activeScreen = 0;        // 0 primary, 1 alternate
    quint16 screenCount = 1;
    std::optional<char32_t> previousCodepoint;  // for REP
    bool cursorIsDefault = true;
    CursorStyle cursorDefaultStyle = CursorBlock;
    quint8 cursorDefaultBlink = 0;   // 0 unset, 1 off, 2 on (as Ghostty: optional bool)
    quint8 shellRedraw = 0;          // 0 full, 1 none, 2 last
    bool modifyOtherKeys2 = false;
    quint8 mouseEvent = 0;           // 0 none, 1 x10, 2 normal, 3 button, 4 any
    quint8 mouseFormat = 0;          // 0 x10, 1 utf8, 2 sgr, 3 urxvt, 4 sgr pixels
    quint8 mouseShiftCapture = 0;    // 0 unset, 1 false, 2 true
    quint8 mouseShape = 0;           // Ghostty's registry, 0 default .. 33 zoom out
    bool passwordInput = false;
    Modes currentModes, savedModes, defaultModes;
    DynamicRgb background, foreground, cursorColor;
    std::optional<quint64> maxScrollbackBytes;  // none: unlimited
    std::optional<quint64> maxScrollbackRows;

    QList<bool> tabStops;            // one per column
    std::array<Rgb, 256> originalPalette{};
    std::array<std::optional<Rgb>, 256> paletteOverrides{};
    QByteArray pwd;                  // as the program set it: usually a file:// URL
    QByteArray title;

    // The palette as currently in effect.
    Rgb palette(int i) const { return paletteOverrides[i].value_or(originalPalette[i]); }
};

// ---- Screens. ---------------------------------------------------------------

struct Charsets
{
    quint8 g[4] = {0, 0, 0, 0};  // 0 utf8, 1 ascii, 2 british, 3 dec special
    quint8 gl = 0, gr = 0;       // slots
    quint8 singleShift = 0;      // 0 none, else 1 + slot
};

struct SavedCursor
{
    quint16 x = 0, y = 0;
    Style pen;
    bool isProtected = false;
    bool pendingWrap = false;
    bool origin = false;
    Charsets charsets;
};

struct KittyPlacementKey
{
    quint32 imageId = 0;
    bool external = false;  // false: internal (the terminal numbered it)
    quint32 id = 0;
    bool operator==(const KittyPlacementKey &o) const { return imageId == o.imageId && external == o.external && id == o.id; }
};

struct KittyPlacement
{
    enum Location : quint8 { Pin = 0, Virtual = 1, Relative = 2 };

    KittyPlacementKey key;
    Location location = Pin;
    // Pin: the cell it is anchored at. In a screen's storage, page indexes
    // the screen's pages; in history it is always the page it came with.
    quint16 page = 0, y = 0, x = 0;
    // Relative: the parent and the offset from it, in cells.
    KittyPlacementKey parent;
    qint32 horizontalOffset = 0, verticalOffset = 0;

    quint32 xOffset = 0, yOffset = 0;  // pixels within the first cell
    quint32 sourceX = 0, sourceY = 0, sourceWidth = 0, sourceHeight = 0;
    quint32 columns = 0, rows = 0;
    qint32 z = 0;
};

enum KittyFormat : quint8 { KittyRgb = 0, KittyRgba = 1, KittyPng = 2, KittyGrayAlpha = 3, KittyGray = 4 };
int kittyBytesPerPixel(KittyFormat f);  // 0 for png

struct KittyFrame
{
    quint32 gapMs = 0;
    quint64 dataLength = 0;  // width * height * 4
    QByteArray data;         // from its KITTY_IMAGE_PAYLOAD, empty if none came
};

struct KittyImage
{
    quint32 id = 0;
    quint32 number = 0;
    quint32 width = 0, height = 0;
    KittyFormat format = KittyRgba;  // never png once stored
    bool transient = false;
    bool implicitId = false;
    quint64 dataLength = 0;          // width * height * bytes per pixel
    QByteArray data;                 // from its KITTY_IMAGE_PAYLOAD (frame 1)

    bool animated = false;
    quint8 animationState = 0;       // 0 stopped, 1 loading, 2 running
    quint32 maxLoops = 0, currentLoop = 0, currentIndex = 0, rootGapMs = 0;
    QList<KittyFrame> frames;        // after the root frame
};

// A chunked transmission cut off between two chunks: the parameters it
// started with and the data so far. Rarely worth resuming.
struct KittyLoading
{
    quint32 id = 0, number = 0, width = 0, height = 0;
    quint8 format = 0, compression = 0, flags = 0, quiet = 0;
    QByteArray data;
};

struct KittyStorage
{
    quint32 nextImageId = 0;
    quint32 nextInternalPlacementId = 0;
    QList<KittyImage> images;          // oldest first
    QList<KittyPlacement> placements;  // parents before children
    std::optional<KittyLoading> loading;
};

struct Screen
{
    quint16 key = 0;             // 0 primary, 1 alternate
    quint64 historyRows = 0;     // complete logical history, for the scrollbar
    quint16 cursorX = 0, cursorY = 0;
    CursorStyle cursorStyle = CursorBlock;
    bool pendingWrap = false;
    bool cursorProtected = false;
    quint8 cursorSemantic = 0;
    bool semanticClearEol = false;
    Style pen;
    quint32 hyperlinkImplicitCounter = 0;
    Charsets charsets;
    quint8 protectedMode = 0;    // 0 off, 1 iso, 2 dec
    quint8 kittyKeyboardIndex = 0;
    std::array<quint8, 8> kittyKeyboardFlags{};
    quint8 semanticClickKind = 0, semanticClickValue = 0;
    std::optional<SavedCursor> savedCursor;
    Hyperlink cursorHyperlink;

    // Oldest to newest; the last `rows` rows are the active area, anything
    // above them in the first page is history already here at READY.
    QList<Page> pages;
    // After READY: older history, newest page first.
    QList<Page> history;
    std::optional<KittyStorage> kitty;

    int pageRows() const;  // rows across `pages`
};

struct Snapshot
{
    TerminalState terminal;
    QList<Screen> screens;       // primary first in canonical encodings
    QByteArray continuation;     // bytes to feed a VT parser in ground state
    qsizetype readyOffset = 0;   // the byte just after READY
    qsizetype length = 0;        // the byte just after FINISH

    const Screen *screen(quint16 key) const;
};

// ---- Entry points. ----------------------------------------------------------

// Where a snapshot at the start of `data` ends. Walks the record framing
// only (no CRCs), so it is cheap enough to call on every network read.
//   > 0   the length through FINISH
//   0     need more bytes
//   < 0   not a snapshot (bad magic or version, or a record over `maxRecord`)
qsizetype scan(QByteArrayView data, quint32 maxRecord = 256u << 20);

struct Result
{
    std::optional<Snapshot> snapshot;
    QString error;  // when there is no snapshot
};

// Decodes one complete snapshot from the start of `data`; bytes after FINISH
// are ignored (the transport's).
Result decode(QByteArrayView data);

// The pieces, for tests and for decoding records found some other way.
// Each reads one schema type from `r`; failures land in `r`.
namespace Decode {
struct RecordHeader { quint16 tag = 0; quint32 length = 0; quint32 crc = 0; };
void envelope(Reader &r);
RecordHeader recordHeader(Reader &r);
// A whole record: header, then exactly `length` payload bytes, CRC checked.
QByteArrayView record(Reader &r, RecordHeader *header);
Color color(Reader &r);
Style style(Reader &r);
Hyperlink hyperlink(Reader &r, bool allowEmpty);
// Rows and the grapheme section of a page of `rows` x `columns`.
void grid(Reader &r, quint16 rows, quint16 columns, Page &page);
void pageHeader(Reader &r, Page &page, quint16 *styleCount, quint16 *hyperlinkCount);
Page page(Reader &r);  // a PAGE payload
void terminalHeader(Reader &r, TerminalState &t);
void terminal(Reader &r, TerminalState &t);  // a TERMINAL payload
// The SCREEN header; returns the page count and whether a saved cursor follows.
quint16 screenHeader(Reader &r, Screen &s, bool *savedCursorPresent);
void screen(Reader &r, Screen &s, quint16 *pageCount);  // a SCREEN payload
SavedCursor savedCursor(Reader &r);
// A HISTORY payload; returns the page count.
quint32 history(Reader &r, quint16 *key);
KittyStorage kittyStorage(Reader &r, quint16 *key);
KittyPlacement kittyPlacement(Reader &r, bool inStorage);
} // namespace Decode

} // namespace GhosttySnapshot
