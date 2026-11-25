/*
 * test-ghosttysnapshot — the Ghostty snapshot decoder against Ghostty's own
 * golden fixtures (src-widgets/dev/ghostty-snapshot/, copied from Ghostty),
 * checking the values Ghostty's tests encoded into each, then against damaged
 * and rearranged copies of them. Prints each failure; exits 1 if there was any.
 *
 *   test-ghosttysnapshot [fixture-dir]
 *   test-ghosttysnapshot --dump file.hex|file.bin   print a decoded snapshot
 */

#include "ghosttysnapshot.h"
#include "ghostty_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

using namespace GhosttySnapshot;

namespace {

int failures = 0;
int checks = 0;
QString current;  // the fixture being checked, for failure messages

void check(bool ok, const char *what, int line)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL line " << line << (current.isEmpty() ? QString() : QStringLiteral(" [") + current + QStringLiteral("]"))
                            << ": " << what << "\n";
        ++failures;
    }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

using Fixture = GhosttyFixture;

QString fixtureDir;

Fixture load(const QString &name)
{
    Fixture f = loadGhosttyFixture(fixtureDir, name);
    if (!f.ok) {
        QTextStream(stderr) << "FAIL: can't read " << QDir(fixtureDir).filePath(name) << "\n";
        ++failures;
    }
    return f;
}

QByteArray body(const Fixture &f) { return f.data.mid(f.offset); }

bool isRgb(const Color &c, int r, int g, int b) { return c.kind == Color::Rgb && c.r == r && c.g == g && c.b == b; }
bool isPalette(const Color &c, int i) { return c.kind == Color::Palette && c.index == i; }
bool isRgb(const std::optional<Rgb> &c, int r, int g, int b) { return c && c->r == r && c->g == g && c->b == b; }

// The text of a row, without trailing blanks.
QString rowText(const Page &p, int y)
{
    QString s;
    const Row &row = p.rowList[y];
    for (int x = 0; x < p.columns; ++x) {
        const Cell c = row.cell(x);
        if (c.width() == Cell::SpacerTail || c.width() == Cell::SpacerHead)
            continue;
        const char32_t cp = c.codepoint();
        s += cp ? QString::fromUcs4(&cp, 1) : QStringLiteral(" ");
        for (char32_t g : p.grapheme(x, y))
            s += QString::fromUcs4(&g, 1);
    }
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

// All rows of a screen, oldest first: history (newest-first on the wire),
// then the SCREEN pages.
QStringList screenText(const Screen &s)
{
    QStringList out;
    for (auto it = s.history.crbegin(); it != s.history.crend(); ++it)
        for (int y = 0; y < it->rows; ++y)
            out << rowText(*it, y);
    for (const Page &p : s.pages)
        for (int y = 0; y < p.rows; ++y)
            out << rowText(p, y);
    return out;
}

// A text form of everything a snapshot decodes to, for comparisons and --dump.
QString describe(const Snapshot &s)
{
    QString out;
    QTextStream o(&out);
    const TerminalState &t = s.terminal;
    o << "terminal " << t.columns << "x" << t.rows << " " << t.widthPx << "x" << t.heightPx << "px"
      << " screens " << t.screenCount << " active " << t.activeScreen
      << " modes 0x" << Qt::hex << t.currentModes.bits << Qt::dec
      << " pwd '" << t.pwd << "' title '" << t.title << "'"
      << " region " << t.scrollTop << "-" << t.scrollBottom << "," << t.scrollLeft << "-" << t.scrollRight << "\n";
    for (int i = 0; i < 256; ++i)
        if (t.paletteOverrides[i])
            o << "  palette " << i << " = " << t.paletteOverrides[i]->r << "," << t.paletteOverrides[i]->g << "," << t.paletteOverrides[i]->b << "\n";
    for (const Screen &sc : s.screens) {
        o << "screen " << sc.key << " cursor " << sc.cursorX << "," << sc.cursorY << " history " << sc.historyRows
          << " pages " << sc.pages.size() << " history pages " << sc.history.size() << "\n";
        for (const QString &line : screenText(sc))
            o << "  |" << line << "|\n";
        auto placements = [&](const QList<KittyPlacement> &list, const char *where) {
            for (const KittyPlacement &p : list) {
                o << "  " << where << " placement " << p.key.imageId << "/" << (p.key.external ? "e" : "i") << p.key.id
                  << " loc " << p.location;
                if (p.location == KittyPlacement::Pin)
                    o << " page " << p.page << " y " << p.y << " x " << p.x;
                if (p.location == KittyPlacement::Relative)
                    o << " parent " << p.parent.imageId << "/" << (p.parent.external ? "e" : "i") << p.parent.id
                      << " H " << p.horizontalOffset << " V " << p.verticalOffset;
                o << " off " << p.xOffset << "," << p.yOffset << " src " << p.sourceX << "," << p.sourceY << " "
                  << p.sourceWidth << "x" << p.sourceHeight << " size " << p.columns << "x" << p.rows << " z " << p.z << "\n";
            }
        };
        if (sc.kitty) {
            const KittyStorage &k = *sc.kitty;
            o << "  kitty next image " << k.nextImageId << " next placement " << k.nextInternalPlacementId << "\n";
            for (const KittyImage &im : k.images) {
                o << "  image " << im.id << " #" << im.number << " " << im.width << "x" << im.height << " format " << im.format
                  << " bytes " << im.dataLength << " data " << im.data.size() << " first " << im.data.left(4).toHex();
                if (im.animated) {
                    o << " anim state " << im.animationState << " loops " << im.maxLoops << " current " << im.currentIndex
                      << " root gap " << im.rootGapMs << " frames";
                    for (const KittyFrame &f : im.frames)
                        o << " [" << f.gapMs << "ms " << f.data.size() << "/" << f.dataLength << " " << f.data.left(4).toHex() << "]";
                }
                o << "\n";
            }
            placements(k.placements, "screen");
            if (k.loading)
                o << "  loading image " << k.loading->id << " " << k.loading->width << "x" << k.loading->height
                  << " format " << k.loading->format << " data " << k.loading->data.toHex() << "\n";
        }
        for (const Page &p : sc.history)
            placements(p.kittyPlacements, "history");
    }
    o << "continuation " << s.continuation.toHex() << " ready at " << s.readyOffset << " length " << s.length << "\n";
    return out;
}

// The records of a snapshot: [start, end) of each, after the envelope.
struct Span { quint16 tag; qsizetype start, end; };
QList<Span> records(const QByteArray &data)
{
    QList<Span> out;
    qsizetype pos = kEnvelopeSize;
    while (pos + kRecordHeaderSize <= data.size()) {
        const quint16 tag = quint16(quint8(data[pos]) | quint8(data[pos + 1]) << 8);
        quint32 len = 0;
        for (int i = 0; i < 4; ++i)
            len |= quint32(quint8(data[pos + 2 + i])) << (8 * i);
        out.append({tag, pos, pos + kRecordHeaderSize + len});
        pos += kRecordHeaderSize + len;
        if (tag == TagFinish)
            break;
    }
    return out;
}

QByteArray frame(quint16 tag, const QByteArray &payload)
{
    QByteArray h(kRecordHeaderSize, 0);
    h[0] = char(tag);
    h[1] = char(tag >> 8);
    for (int i = 0; i < 4; ++i)
        h[2 + i] = char(quint32(payload.size()) >> (8 * i));
    const quint32 crc = crc32c(payload, crc32c(QByteArrayView(h.constData(), 6)));
    for (int i = 0; i < 4; ++i)
        h[6 + i] = char(crc >> (8 * i));
    return h + payload;
}

// Decodes a fixture's bytes with the decoder its Kaitai type names.
bool decodeAs(const Fixture &f)
{
    const QByteArray bytes = body(f);
    Reader r(bytes);
    Page page;
    if (f.type == QLatin1String("envelope")) {
        Decode::envelope(r);
    } else if (f.type == QLatin1String("record_header")) {
        const auto h = Decode::recordHeader(r);
        CHECK(h.tag == f.params.value(0).toUInt());
    } else if (f.type == QLatin1String("checkpoint_record") || f.type == QLatin1String("continuation_record")
               || f.type == QLatin1String("page_record")) {
        Decode::RecordHeader h;
        const QByteArrayView payload = Decode::record(r, &h);
        const quint16 want = f.type == QLatin1String("checkpoint_record") ? f.params.value(0).toUShort()
            : f.type == QLatin1String("page_record") ? quint16(TagPage) : quint16(TagContinuation);
        CHECK(h.tag == want);
        if (r.ok() && h.tag == TagPage) {
            Reader pr(payload);
            Decode::page(pr);
            CHECK(pr.ok());
        }
    } else if (f.type == QLatin1String("grid")) {
        page.rows = f.params.value(0).toUShort();
        page.columns = f.params.value(1).toUShort();
        Decode::grid(r, page.rows, page.columns, page);
    } else if (f.type == QLatin1String("history_payload")) {
        Decode::history(r, nullptr);
    } else if (f.type == QLatin1String("hyperlink")) {
        Decode::hyperlink(r, f.params.value(1) == QLatin1String("true"));
    } else if (f.type == QLatin1String("page_header")) {
        quint16 a, b;
        Decode::pageHeader(r, page, &a, &b);
    } else if (f.type == QLatin1String("page_payload")) {
        Decode::page(r);
    } else if (f.type == QLatin1String("screen_header")) {
        Screen s;
        Decode::screenHeader(r, s, nullptr);
    } else if (f.type == QLatin1String("saved_cursor")) {
        Decode::savedCursor(r);
    } else if (f.type == QLatin1String("style")) {
        Decode::style(r);
    } else if (f.type == QLatin1String("terminal_header")) {
        TerminalState t;
        Decode::terminalHeader(r, t);
    } else if (f.type == QLatin1String("ghostty_snapshot")) {
        const Result res = decode(bytes);
        if (!res.snapshot)
            QTextStream(stderr) << "  " << f.name << ": " << res.error << "\n";
        return res.snapshot && res.snapshot->length == bytes.size();
    } else {
        QTextStream(stderr) << "  " << f.name << ": no decoder for type " << f.type << "\n";
        return false;
    }
    if (!r.ok())
        QTextStream(stderr) << "  " << f.name << ": " << r.error() << "\n";
    return r.ok() && r.atEnd();
}

std::optional<Snapshot> snapshotOf(const QString &name)
{
    const Result res = decode(load(name).data);
    if (!res.snapshot)
        QTextStream(stderr) << "  " << name << ": " << res.error << "\n";
    return res.snapshot;
}

const KittyPlacement *findPlacement(const QList<KittyPlacement> &list, quint32 image, bool external, quint32 id)
{
    for (const KittyPlacement &p : list)
        if (p.key.imageId == image && p.key.external == external && p.key.id == id)
            return &p;
    return nullptr;
}

int dump(const QString &path)
{
    QByteArray data;
    if (path.endsWith(QLatin1String(".hex"))) {
        fixtureDir = QFileInfo(path).absolutePath();
        data = body(load(QFileInfo(path).fileName()));
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return 2;
        data = f.readAll();
    }
    const Result res = decode(data);
    if (!res.snapshot) {
        QTextStream(stderr) << res.error << "\n";
        return 1;
    }
    QTextStream(stdout) << describe(*res.snapshot);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == QLatin1String("--dump"))
        return dump(args[2]);
    fixtureDir = args.size() > 1 ? args[1] : QStringLiteral(PJ_GHOSTTY_FIXTURES);

    // ---- Every fixture decodes as the type it says it holds, exactly. ----
    const QStringList names = QDir(fixtureDir).entryList({QStringLiteral("*.hex")}, QDir::Files, QDir::Name);
    CHECK(names.size() == 32);
    for (const QString &name : names) {
        current = name;
        const Fixture f = load(name);
        CHECK(!f.type.isEmpty());
        CHECK(decodeAs(f));
    }

    // ---- CRC32C ----
    current = QStringLiteral("crc32c");
    CHECK(crc32c("123456789") == 0xE3069283u);  // the standard check value
    {
        // Ghostty's PAGE record header test: tag 3, 24 payload bytes.
        QByteArray covered("\x03\x00\x18\x00\x00\x00", 6);
        covered += QByteArray("\x50\x00\x18\x00", 4) + QByteArray(20, '\0');
        CHECK(crc32c(covered) == 0x7178441bu);
        const QByteArray rBytes = load(QStringLiteral("record-page-header-v1.hex")).data;
        Reader r(rBytes);
        const auto h = Decode::recordHeader(r);
        CHECK(h.tag == TagPage && h.length == 24 && h.crc == 0x7178441bu);
    }

    // ---- style-v1 ----
    current = QStringLiteral("style-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("style-v1.hex")).data;
        Reader r(rBytes);
        const Style s = Decode::style(r);
        CHECK(s.foreground.kind == Color::None);
        CHECK(isPalette(s.background, 0x7f));
        CHECK(isRgb(s.underlineColor, 0x12, 0x34, 0x56));
        CHECK(s.flags == 0xFF);
        CHECK(s.underline == Style::Curly);
    }

    // ---- hyperlinks ----
    current = QStringLiteral("hyperlink");
    {
        const QByteArray rBytes = load(QStringLiteral("hyperlink-implicit-v1.hex")).data;
        Reader r(rBytes);
        const Hyperlink h = Decode::hyperlink(r, false);
        CHECK(h.kind == Hyperlink::Implicit && h.implicitId == 0x01020304 && h.uri == "uri");
        const QByteArray eBytes = load(QStringLiteral("hyperlink-explicit-v1.hex")).data;
        Reader e(eBytes);
        const Hyperlink x = Decode::hyperlink(e, false);
        CHECK(x.kind == Hyperlink::Explicit && x.id == "id" && x.uri == "uri");
    }

    // ---- page-header-v1 ----
    current = QStringLiteral("page-header-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("page-header-v1.hex")).data;
        Reader r(rBytes);
        Page p;
        quint16 styles = 0, links = 0;
        Decode::pageHeader(r, p, &styles, &links);
        CHECK(p.columns == 0x0102 && p.rows == 0x0304);
        CHECK(styles == 0x0506 && links == 0x0708);
        CHECK(p.styleCapacity == 0x090a && p.hyperlinkCapacityBytes == 0x0b0c);
        CHECK(p.graphemeCapacityBytes == 0x0d0e0f10u && p.stringCapacityBytes == 0x11121314u);
    }

    // ---- history-header-v1 ----
    current = QStringLiteral("history-header-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("history-header-v1.hex")).data;
        Reader r(rBytes);
        quint16 key = 0;
        CHECK(Decode::history(r, &key) == 0x01020304u);
        CHECK(key == 1);
    }

    // ---- terminal-header-v1 ----
    current = QStringLiteral("terminal-header-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("terminal-header-v1.hex")).data;
        Reader r(rBytes);
        TerminalState t;
        Decode::terminalHeader(r, t);
        CHECK(r.ok());
        CHECK(t.columns == 0x0102 && t.rows == 0x0304);
        CHECK(t.widthPx == 0x05060708u && t.heightPx == 0x090a0b0cu);
        CHECK(t.scrollTop == 1 && t.scrollBottom == 2 && t.scrollLeft == 3 && t.scrollRight == 4);
        CHECK(t.statusDisplay == 1);
        CHECK(t.activeScreen == 1 && t.screenCount == 2);
        CHECK(t.previousCodepoint == char32_t('A'));
        CHECK(t.cursorIsDefault && t.cursorDefaultStyle == CursorBlockHollow && t.cursorDefaultBlink == 2);
        CHECK(t.shellRedraw == 2 && t.modifyOtherKeys2);
        CHECK(t.mouseEvent == 4 && t.mouseFormat == 4 && t.mouseShiftCapture == 1 && t.mouseShape == 33);
        CHECK(t.passwordInput);
        CHECK(t.currentModes.bits == 1);
        CHECK(t.currentModes.has(ModeDisableKeyboard) && !t.currentModes.has(ModeInBandSizeReports));
        CHECK(t.savedModes.bits == (quint64(1) << ModeInBandSizeReports));
        CHECK(t.defaultModes.has(ModeDisableKeyboard) && t.defaultModes.has(ModeInBandSizeReports));
        CHECK(isRgb(t.background.defaultValue, 1, 2, 3) && !t.background.override);
        CHECK(!t.foreground.defaultValue && isRgb(t.foreground.override, 4, 5, 6));
        CHECK(isRgb(t.cursorColor.defaultValue, 7, 8, 9) && isRgb(t.cursorColor.override, 10, 11, 12));
        CHECK(!t.maxScrollbackBytes);
        CHECK(t.maxScrollbackRows == quint64(0x0102030405060708));
    }

    // ---- screen-header-v1 and screen-saved-cursor-v1 ----
    current = QStringLiteral("screen-header-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("screen-header-v1.hex")).data;
        Reader r(rBytes);
        Screen s;
        bool saved = false;
        CHECK(Decode::screenHeader(r, s, &saved) == 0x0203);
        CHECK(r.ok() && r.atEnd());
        CHECK(s.key == 1 && s.historyRows == quint64(0x0102030405060708));
        CHECK(s.cursorX == 0x0405 && s.cursorY == 0x0607);
        CHECK(s.cursorStyle == CursorBlockHollow);
        CHECK(s.pendingWrap && !s.cursorProtected && s.cursorSemantic == 2 && s.semanticClearEol);
        CHECK(isPalette(s.pen.background, 0x7f) && s.pen.flags == 0xFF && s.pen.underline == Style::Curly);
        CHECK(s.hyperlinkImplicitCounter == 0x0a0b0c0du);
        CHECK(s.charsets.g[0] == 0 && s.charsets.g[1] == 1 && s.charsets.g[2] == 2 && s.charsets.g[3] == 3);
        CHECK(s.charsets.gl == 1 && s.charsets.gr == 3 && s.charsets.singleShift == 3);  // G2, plus one
        CHECK(s.protectedMode == 2);
        CHECK(s.kittyKeyboardIndex == 7);
        const std::array<quint8, 8> flags{1, 2, 4, 8, 16, 31, 0, 17};
        CHECK(s.kittyKeyboardFlags == flags);
        CHECK(s.semanticClickKind == 2 && s.semanticClickValue == 3);  // cl, smart_vertical
        CHECK(saved);
    }
    current = QStringLiteral("screen-saved-cursor-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("screen-saved-cursor-v1.hex")).data;
        Reader r(rBytes);
        const SavedCursor c = Decode::savedCursor(r);
        CHECK(r.ok() && r.atEnd());
        CHECK(c.x == 0x0102 && c.y == 0x0304);
        CHECK(c.pen == Style());
        CHECK(c.isProtected && c.pendingWrap && c.origin);
        CHECK(c.charsets.gl == 1 && c.charsets.gr == 3 && c.charsets.g[3] == 3);
    }

    // ---- grid-v1: 4 rows of 3 ----
    current = QStringLiteral("grid-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("grid-v1.hex")).data;
        Reader r(rBytes);
        Page p;
        p.rows = 4;
        p.columns = 3;
        Decode::grid(r, 4, 3, p);
        CHECK(r.ok() && r.atEnd());
        CHECK(p.rowList.size() == 4);
        if (p.rowList.size() == 4) {
            const Row &r0 = p.rowList[0];
            CHECK(r0.prompt == Row::PromptStart && !r0.wrap);
            CHECK(r0.cell(0).codepoint() == 'A' && r0.cell(0).width() == Cell::Wide);
            CHECK(r0.cell(0).isProtected() && r0.cell(0).semantic() == Cell::Prompt);
            CHECK(r0.cell(1).width() == Cell::SpacerTail && r0.cell(1).semantic() == Cell::Input);
            CHECK(r0.cell(2).codepoint() == 'x');
            CHECK(p.grapheme(2, 0) == QList<char32_t>({0x0301, 0x0302}));
            const Row &r1 = p.rowList[1];
            CHECK(r1.wrap && r1.wrapContinuation && r1.prompt == Row::PromptContinuation);
            CHECK(r1.cell(0).content() == Cell::PaletteBackground && r1.cell(0).paletteIndex() == 7);
            CHECK(r1.cell(1).content() == Cell::RgbBackground && r1.cell(1).rgb() == 0xaabbccu && r1.cell(1).isProtected());
            CHECK(r1.cell(2).width() == Cell::SpacerHead);
            CHECK(rowText(p, 2) == QLatin1String("h i"));
            CHECK(p.rowList[2].cells.size() == 3);
            CHECK(rowText(p, 3) == QString::fromUtf8("Ж!"));
            CHECK(p.rowList[3].cells.size() == 2);  // trailing default cell elided
            CHECK(p.rowList[3].cell(2).word == 0);
        }
    }

    // ---- page-v1: 3x2 with styles, hyperlinks and a grapheme ----
    current = QStringLiteral("page-v1");
    {
        const QByteArray rBytes = load(QStringLiteral("page-v1.hex")).data;
        Reader r(rBytes);
        const Page p = Decode::page(r);
        CHECK(r.ok());
        CHECK(p.columns == 3 && p.rows == 2);
        CHECK(p.styles.size() == 2 && p.hyperlinks.size() == 2);
        CHECK(p.styleCapacity == 8 && p.hyperlinkCapacityBytes == 512 && p.graphemeCapacityBytes == 128 && p.stringCapacityBytes == 256);
        if (p.rowList.size() == 2) {
            const Row &r0 = p.rowList[0];
            const Cell a = r0.cell(0), b = r0.cell(1), c = r0.cell(2);
            CHECK(a.codepoint() == 'A' && a.width() == Cell::Wide && a.isProtected() && a.semantic() == Cell::Prompt);
            CHECK(p.styles.value(a.styleId()).has(Style::Bold));
            CHECK(p.hyperlinks.value(a.hyperlinkId()).uri == "alpha" && p.hyperlinks.value(a.hyperlinkId()).id == "a");
            CHECK(b.width() == Cell::SpacerTail && b.semantic() == Cell::Input);
            CHECK(isPalette(p.styles.value(b.styleId()).background, 42));
            const Hyperlink hb = p.hyperlinks.value(b.hyperlinkId());
            CHECK(hb.kind == Hyperlink::Implicit && hb.implicitId == 0x01020304 && hb.uri == "beta");
            CHECK(c.content() == Cell::PaletteBackground && c.paletteIndex() == 7);
            CHECK(c.styleId() == a.styleId() && c.hyperlinkId() == a.hyperlinkId());
            const Row &r1 = p.rowList[1];
            CHECK(r1.wrap && r1.wrapContinuation && r1.prompt == Row::PromptContinuation);
            CHECK(r1.cell(0).codepoint() == 'x' && p.grapheme(0, 1) == QList<char32_t>({0x0301, 0x0302}));
            CHECK(r1.cell(1).rgb() == 0xaabbccu && r1.cell(1).isProtected());
            CHECK(r1.cell(2).width() == Cell::SpacerHead);
        }
    }

    // ---- continuations: the half-parsed sequences ----
    current = QStringLiteral("continuation");
    {
        const QList<std::pair<const char *, QByteArray>> cases = {
            {"continuation-ground-v1.hex", QByteArray()},
            {"continuation-utf8-v1.hex", QByteArray("\xf0\x9f\x98")},
            {"continuation-esc-v1.hex", QByteArray("\x1b")},
            {"continuation-csi-v1.hex", QByteArray("\x1b[31")},
            {"continuation-osc-v1.hex", QByteArray("\x1b]2;title")},
            {"continuation-dcs-v1.hex", QByteArray("\x1bPqdata")},
            {"continuation-apc-v1.hex", QByteArray("\x1b_Gdata")},
        };
        for (const auto &[name, bytes] : cases) {
            current = QLatin1String(name);
            const QByteArray rBytes = load(QLatin1String(name)).data;
        Reader r(rBytes);
            Decode::RecordHeader h;
            const QByteArrayView payload = Decode::record(r, &h);
            CHECK(r.ok() && h.tag == TagContinuation && payload == bytes);
        }
    }

    // ---- checkpoint-ready-v1: after a three-byte prefix ----
    current = QStringLiteral("checkpoint-ready-v1");
    {
        const Fixture f = load(QStringLiteral("checkpoint-ready-v1.hex"));
        CHECK(f.data.startsWith("abc"));
        const QByteArray rBytes = body(f);
        Reader r(rBytes);
        Decode::RecordHeader h;
        Decode::record(r, &h);
        CHECK(r.ok() && h.tag == TagReady && h.length == 0);
    }

    // ---- complete-v1: a 2x3 terminal, history, an alternate screen ----
    current = QStringLiteral("complete-v1");
    if (const auto s = snapshotOf(QStringLiteral("complete-v1.hex"))) {
        const TerminalState &t = s->terminal;
        CHECK(t.columns == 2 && t.rows == 3);
        CHECK(t.widthPx == 800 && t.heightPx == 600);
        CHECK(t.palette(7) == (Rgb{1, 2, 3}));
        CHECK(t.currentModes.has(ModeBracketedPaste));
        CHECK(t.pwd == "file:///tmp/snapshot" && t.title == "complete snapshot");
        CHECK(t.screenCount == 2 && t.activeScreen == 1);
        const Screen *primary = s->screen(0);
        const Screen *alternate = s->screen(1);
        CHECK(primary && alternate);
        if (primary) {
            CHECK(screenText(*primary) == QStringList({"A", "", "B", "", "C", "D", "E"}));
            CHECK(primary->historyRows == 4);
            CHECK(primary->history.size() == 2);
            // History pages come newest first.
            CHECK(primary->history.size() == 2 && rowText(primary->history[0], 0) == QLatin1String("B"));
        }
        if (alternate) {
            // "alternate" printed into 2x3, scrolled with no scrollback.
            CHECK(alternate->history.isEmpty() && alternate->historyRows == 0);
            CHECK(screenText(*alternate) == QStringList({"rn", "at", "e"}));
            CHECK(alternate->cursorX == 1 && alternate->cursorY == 2);
        }
        CHECK(s->continuation.isEmpty());
        CHECK(s->readyOffset > 0 && s->readyOffset < s->length);
    }

    // ---- Kitty graphics ----
    current = QStringLiteral("kitty-placed-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-placed-v1.hex"))) {
        // a=T,f=32,s=8,v=8,i=1,c=2,r=1,C=1 of red pixels
        const Screen *sc = s->screen(0);
        CHECK(sc && sc->kitty && sc->kitty->images.size() == 1 && sc->kitty->placements.size() == 1);
        if (sc && sc->kitty && sc->kitty->images.size() == 1 && sc->kitty->placements.size() == 1) {
            const KittyImage &im = sc->kitty->images[0];
            CHECK(im.id == 1 && im.width == 8 && im.height == 8 && im.format == KittyRgba);
            CHECK(im.dataLength == 256 && im.data.size() == 256);
            CHECK(im.data.left(4) == QByteArray("\xff\x00\x00\xff", 4));
            const KittyPlacement &p = sc->kitty->placements[0];
            CHECK(p.key.imageId == 1 && p.location == KittyPlacement::Pin && p.columns == 2 && p.rows == 1);
        }
    }
    current = QStringLiteral("kitty-source-rect-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-source-rect-v1.hex"))) {
        // i=2,p=7,x=1,y=1,w=2,h=2,X=3,Y=4,z=-5
        const Screen *sc = s->screen(0);
        const KittyPlacement *p = sc && sc->kitty ? findPlacement(sc->kitty->placements, 2, true, 7) : nullptr;
        CHECK(p);
        if (p) {
            CHECK(p->sourceX == 1 && p->sourceY == 1 && p->sourceWidth == 2 && p->sourceHeight == 2);
            CHECK(p->xOffset == 3 && p->yOffset == 4 && p->z == -5);
        }
    }
    current = QStringLiteral("kitty-virtual-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-virtual-v1.hex"))) {
        // U=1,i=3,c=2,r=1, then two placeholder cells in colour 3
        const Screen *sc = s->screen(0);
        CHECK(sc && sc->kitty && sc->kitty->placements.size() == 1);
        if (sc && sc->kitty && sc->kitty->placements.size() == 1) {
            const KittyPlacement &p = sc->kitty->placements[0];
            CHECK(p.key.imageId == 3 && p.location == KittyPlacement::Virtual && p.columns == 2 && p.rows == 1);
            const Page &page = sc->pages.last();
            const int y = page.rows - s->terminal.rows;
            CHECK(page.rowList[y].cell(0).codepoint() == 0x10EEEE && page.rowList[y].cell(1).codepoint() == 0x10EEEE);
            CHECK(page.grapheme(0, y) == QList<char32_t>({0x0305, 0x0305}));
            CHECK(page.grapheme(1, y) == QList<char32_t>({0x0305, 0x030D}));
            CHECK(isPalette(page.styles.value(page.rowList[y].cell(0).styleId()).foreground, 3));
        }
    }
    current = QStringLiteral("kitty-relative-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-relative-v1.hex"))) {
        const Screen *sc = s->screen(0);
        CHECK(sc && sc->kitty && sc->kitty->placements.size() == 3);
        if (sc && sc->kitty) {
            const auto &list = sc->kitty->placements;
            const KittyPlacement *five = findPlacement(list, 2, true, 5);
            const KittyPlacement *six = findPlacement(list, 1, true, 6);
            CHECK(five && five->location == KittyPlacement::Relative && five->parent.imageId == 1
                  && five->horizontalOffset == 1 && five->verticalOffset == 1);
            CHECK(six && six->location == KittyPlacement::Relative && six->parent.imageId == 2
                  && six->parent.external && six->parent.id == 5 && six->horizontalOffset == -1 && six->verticalOffset == 1);
            // Parents come before children.
            int root = -1, mid = -1, leaf = -1;
            for (int i = 0; i < list.size(); ++i) {
                if (list[i].location == KittyPlacement::Pin) root = i;
                if (&list[i] == five) mid = i;
                if (&list[i] == six) leaf = i;
            }
            CHECK(root >= 0 && root < mid && mid < leaf);
        }
    }
    current = QStringLiteral("kitty-history-v1");
    std::optional<Snapshot> history = snapshotOf(QStringLiteral("kitty-history-v1.hex"));
    if (history) {
        // Placements on both history pages and the active page, and a
        // relative child of the oldest page's.
        const Screen *sc = history->screen(0);
        CHECK(sc && sc->history.size() == 2);
        if (sc && sc->history.size() == 2 && sc->kitty) {
            CHECK(screenText(*sc) == QStringList({"A", "", "B", "", "C", ""}));
            const Page &newest = sc->history[0], &oldest = sc->history[1];
            CHECK(rowText(oldest, 0) == QLatin1String("A") && rowText(newest, 0) == QLatin1String("B"));
            const KittyPlacement *one = findPlacement(oldest.kittyPlacements, 4, true, 1);
            CHECK(one && one->location == KittyPlacement::Pin && one->y == 1 && one->x == 1);
            const KittyPlacement *nine = findPlacement(oldest.kittyPlacements, 4, true, 9);
            CHECK(nine && nine->location == KittyPlacement::Relative && nine->parent.id == 1 && nine->horizontalOffset == 1);
            const KittyPlacement *two = findPlacement(newest.kittyPlacements, 4, true, 2);
            CHECK(two && two->y == 0 && two->x == 0);
            const KittyPlacement *three = findPlacement(sc->kitty->placements, 4, true, 3);
            CHECK(three && three->location == KittyPlacement::Pin && three->y == 1 && three->x == 0);
            CHECK(sc->kitty->images.size() == 1 && sc->kitty->images[0].data == QByteArray("\x09\x09\x09\xff", 4));
        }
    }
    current = QStringLiteral("kitty-animation-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-animation-v1.hex"))) {
        // 2x1 red root, a 50 ms green frame, a 1x1 blue frame composed onto
        // a 70 ms frame; root gap 30; running at frame 2 (index 1).
        const Screen *sc = s->screen(0);
        CHECK(sc && sc->kitty && sc->kitty->images.size() == 1);
        if (sc && sc->kitty && sc->kitty->images.size() == 1) {
            const KittyImage &im = sc->kitty->images[0];
            CHECK(im.id == 5 && im.animated && im.animationState == 2 && im.currentIndex == 1);
            CHECK(im.rootGapMs == 30 && im.frames.size() == 2);
            CHECK(im.maxLoops == 2);  // v=3: kitty counts plays, Ghostty stores loops
            CHECK(im.data == QByteArray("\xff\x00\x00\xff\xff\x00\x00\xff", 8));
            if (im.frames.size() == 2) {
                CHECK(im.frames[0].gapMs == 50 && im.frames[0].data == QByteArray("\x00\xff\x00\xff\x00\xff\x00\xff", 8));
                CHECK(im.frames[1].gapMs == 70 && im.frames[1].data.size() == 8);
                CHECK(im.frames[1].data.left(4) == QByteArray("\x00\x00\xff\xff", 4));
            }
        }
    }
    current = QStringLiteral("kitty-loading-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-loading-v1.hex"))) {
        const Screen *sc = s->screen(0);
        CHECK(sc && sc->kitty && sc->kitty->loading);
        if (sc && sc->kitty && sc->kitty->loading) {
            const KittyLoading &l = *sc->kitty->loading;
            CHECK(l.id == 6 && l.width == 2 && l.height == 2);
            CHECK(l.data == QByteArray("\x07\x07\x07\xff\x07\x07", 6));
        }
    }
    current = QStringLiteral("kitty-alternate-v1");
    if (const auto s = snapshotOf(QStringLiteral("kitty-alternate-v1.hex"))) {
        // Image 1 on each screen: RGBA 1,1,1 on the primary, RGB 2,2,2 on the alternate.
        const Screen *p = s->screen(0), *a = s->screen(1);
        CHECK(p && p->kitty && p->kitty->images.size() == 1 && p->kitty->images[0].format == KittyRgba
              && p->kitty->images[0].data == QByteArray("\x01\x01\x01\xff", 4));
        CHECK(a && a->kitty && a->kitty->images.size() == 1 && a->kitty->images[0].format == KittyRgb
              && a->kitty->images[0].data == QByteArray("\x02\x02\x02", 3));
        CHECK(s->terminal.activeScreen == 1);
    }

    // ---- Unknown records anywhere change nothing ----
    current = QStringLiteral("unknown-records-v1");
    if (const auto s = snapshotOf(QStringLiteral("unknown-records-v1.hex")); s && history) {
        Snapshot a = *s, b = *history;
        a.readyOffset = b.readyOffset = a.length = b.length = 0;
        CHECK(describe(a) == describe(b));
    }

    // ---- The payload may come anywhere after READY, except between a
    // history PAGE and its placements ----
    current = QStringLiteral("payload moves");
    {
        const QByteArray data = load(QStringLiteral("kitty-history-v1.hex")).data;
        const QList<Span> recs = records(data);
        QList<Span> others;
        Span payload{};
        for (const Span &r : recs) {
            if (r.tag == TagKittyImagePayload)
                payload = r;
            else
                others.append(r);
        }
        int ready = -1;
        for (int i = 0; i < others.size(); ++i)
            if (others[i].tag == TagReady)
                ready = i;
        CHECK(payload.tag == TagKittyImagePayload && ready > 0);
        const QString want = history ? describe(*history).section(QLatin1String("continuation"), 0, 0) : QString();
        int moved = 0, refused = 0;
        for (int pos = ready + 1; pos < others.size(); ++pos) {
            QByteArray out = data.left(kEnvelopeSize);
            for (int i = 0; i < others.size(); ++i) {
                if (i == pos)
                    out += data.mid(payload.start, payload.end - payload.start);
                out += data.mid(others[i].start, others[i].end - others[i].start);
            }
            const Result res = decode(out);
            if (others[pos].tag == TagKittyImagePagePlacements) {
                CHECK(!res.snapshot);
                ++refused;
            } else {
                CHECK(res.snapshot && describe(*res.snapshot).section(QLatin1String("continuation"), 0, 0) == want);
                ++moved;
            }
        }
        CHECK(moved > 3 && refused == 2);
        // The fixture Ghostty made of one such move decodes the same too.
        if (const auto s = snapshotOf(QStringLiteral("kitty-payload-in-history-v1.hex")))
            CHECK(describe(*s).section(QLatin1String("continuation"), 0, 0) == want);
    }

    // ---- Without its Kitty records a snapshot is still a snapshot ----
    current = QStringLiteral("kitty stripped");
    for (const char *name : {"kitty-placed-v1.hex", "kitty-history-v1.hex", "kitty-animation-v1.hex",
                             "kitty-loading-v1.hex", "kitty-alternate-v1.hex", "kitty-relative-v1.hex"}) {
        current = QStringLiteral("kitty stripped ") + QLatin1String(name);
        const QByteArray data = load(QLatin1String(name)).data;
        QByteArray out = data.left(kEnvelopeSize);
        for (const Span &r : records(data))
            if (r.tag < TagKittyImageStorage)
                out += data.mid(r.start, r.end - r.start);
        const Result res = decode(out);
        CHECK(res.snapshot);
        if (res.snapshot)
            for (const Screen &sc : res.snapshot->screens)
                CHECK(!sc.kitty && std::all_of(sc.history.cbegin(), sc.history.cend(),
                                               [](const Page &p) { return p.kittyPlacements.isEmpty(); }));
    }

    // ---- scan(): framing only ----
    current = QStringLiteral("scan");
    {
        const QByteArray data = load(QStringLiteral("complete-v1.hex")).data;
        CHECK(scan(data) == data.size());
        CHECK(scan(data + "live bytes") == data.size());
        CHECK(scan(QByteArray()) == 0);
        CHECK(scan("GHOST") == 0);
        CHECK(scan("HELLO") < 0);
        CHECK(scan(QByteArray("GHOSTSNP\x02\x00", 10)) < 0);
        bool prefixes = true;
        for (qsizetype n = 0; n < data.size(); ++n)
            prefixes = prefixes && scan(data.left(n)) == 0;
        CHECK(prefixes);
        // A record longer than the limit is refused, not waited for.
        QByteArray big = data.left(kEnvelopeSize) + frame(0x4000, QByteArray(64, 'x'));
        CHECK(scan(big, 32) < 0);
        CHECK(scan(big) == 0);
        // decode() ignores what follows FINISH, and says where FINISH ended.
        const Result res = decode(data + "trailing");
        CHECK(res.snapshot && res.snapshot->length == data.size());
    }

    // ---- Damage: every truncation fails; every flipped byte fails or
    // decodes; nothing crashes (run under ASan/UBSan to mean it) ----
    current = QStringLiteral("damage");
    for (const QString &name : names) {
        const Fixture f = load(name);
        if (f.type != QLatin1String("ghostty_snapshot"))
            continue;
        current = QStringLiteral("damage ") + name;
        const QByteArray data = f.data;
        bool truncations = true;
        for (qsizetype n = 0; n < data.size(); ++n)
            truncations = truncations && !decode(data.left(n)).snapshot;
        CHECK(truncations);
        int decoded = 0;
        for (qsizetype i = 0; i < data.size(); ++i) {
            QByteArray bad = data;
            bad[i] = char(bad[i] ^ 0x5A);
            if (decode(bad).snapshot)
                ++decoded;
        }
        // Only a flipped byte in an unknown record's tag could survive
        // (it becomes another unknown tag); everything else is CRC-covered.
        CHECK(decoded == 0 || name == QLatin1String("unknown-records-v1.hex"));
    }

    // ---- Lenient values, strict structure ----
    current = QStringLiteral("lenient");
    {
        // A cell naming a style and hyperlink the page doesn't have, a
        // surrogate codepoint, a wide cell with no tail and a reserved
        // semantic value: all degrade.
        QByteArray page;
        auto u16 = [&](quint16 v) { page += char(v); page += char(v >> 8); };
        auto u32 = [&](quint32 v) { u16(quint16(v)); u16(quint16(v >> 16)); };
        auto u64 = [&](quint64 v) { u32(quint32(v)); u32(quint32(v >> 32)); };
        u16(3); u16(1); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);  // header
        page += char(3 << 4);  // row flags: eight-byte cells
        u16(3);
        u64((quint64(0xD800) << 2) | (quint64(9) << 26) | (quint64(1) << 45) | (quint64(7) << 48));
        u64((quint64('w') << 2) | (quint64(Cell::Wide) << 42) | (quint64(3) << 46));
        u64(quint64('z') << 2);
        u32(1);  // one grapheme entry, for a cell off the page: ignored
        u16(9); u16(9); u16(1); u32(0x301);
        Reader r(page);
        const Page p = Decode::page(r);
        CHECK(r.ok());
        if (r.ok() && p.rowList.size() == 1) {
            const Row &row = p.rowList[0];
            CHECK(row.cell(0).codepoint() == 0xFFFD);
            CHECK(row.cell(0).styleId() == 0 && row.cell(0).hyperlinkId() == 0);
            CHECK(row.cell(1).width() == Cell::Narrow);
            CHECK(row.cell(1).semantic() == Cell::Output);
            CHECK(p.graphemes.isEmpty());
        }
        // More cells than columns misframes everything after: refused.
        QByteArray tooMany = page;
        tooMany[20 + 1] = 4;
        Reader r2(tooMany);
        Decode::page(r2);
        CHECK(!r2.ok());
        // An unknown hyperlink kind can't be skipped: refused.
        QByteArray link("\x07\x00\x00\x00\x00", 5);
        Reader r3(link);
        Decode::hyperlink(r3, true);
        CHECK(!r3.ok());
        // A huge declared count is refused before anything is allocated.
        QByteArray storage(19, '\0');
        storage[10] = '\xff'; storage[11] = '\xff'; storage[12] = '\xff'; storage[13] = '\x7f';
        Reader r4(storage);
        Decode::kittyStorage(r4, nullptr);
        CHECK(!r4.ok());
    }

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
