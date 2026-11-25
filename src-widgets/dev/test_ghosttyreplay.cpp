/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-ghosttyreplay: Ghostty snapshots replayed into qtermwidget's own
 * emulator (no window), and what it ends up with compared with the snapshot:
 * every line of history and screen, wrapped lines, the cursor, which screen
 * is showing, the title, Kitty images and where they are placed. Run:
 * ./build/test-ghosttyreplay [fixture dir]
 */

#include "ghosttyreplay.h"
#include "ghostty_fixture.h"
#include "qtermprobe.h"

#include "Character.h"
#include "History.h"
#include "Screen.h"
#include "Vt102Emulation.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

using namespace GhosttySnapshot;

namespace {

int failures = 0;
int checks = 0;
QString current;

void check(bool ok, const char *what, int line)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL line " << line
                            << (current.isEmpty() ? QString() : QStringLiteral(" [") + current + QStringLiteral("]"))
                            << ": " << what << "\n";
        ++failures;
    }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
#define CHECK_EQ(a, b) checkEq((a), (b), #a " == " #b, __LINE__)

template<typename A, typename B>
void checkEq(const A &a, const B &b, const char *what, int line)
{
    const bool ok = a == b;
    check(ok, what, line);
    if (!ok)
        QTextStream(stderr) << "    got " << QVariant::fromValue(a).toString() << ", want "
                            << QVariant::fromValue(b).toString() << "\n";
}

void checkLines(const QStringList &got, const QStringList &want, int line)
{
    check(got == want, "lines match", line);
    if (got != want) {
        QTextStream err(stderr);
        err << "    got:\n";
        for (const QString &l : got)
            err << "      |" << l << "|\n";
        err << "    want:\n";
        for (const QString &l : want)
            err << "      |" << l << "|\n";
    }
}

QString fixtureDir;

using Emulator = QtermProbe;
using QtermProbeLines::screenLines;

struct Expected
{
    QStringList lines;
    QList<bool> wrapped;
};

// What a snapshot screen shows, the same way: history (if wanted) and the
// active area, as text.
Expected snapshotLines(const Snapshot &snap, const GhosttySnapshot::Screen &s, bool withHistory)
{
    QList<std::pair<const Page *, int>> rows;
    if (withHistory)
        for (auto it = s.history.crbegin(); it != s.history.crend(); ++it)
            for (int y = 0; y < it->rowList.size(); ++y)
                rows.append({&*it, y});
    for (const Page &p : s.pages)
        for (int y = 0; y < p.rowList.size(); ++y)
            rows.append({&p, y});
    if (!withHistory)
        rows = rows.mid(std::max<qsizetype>(0, rows.size() - snap.terminal.rows));

    Expected e;
    for (const auto &[page, y] : rows) {
        const Row &row = page->rowList[y];
        QString line;
        for (int x = 0; x < snap.terminal.columns; ++x) {
            const Cell c = row.cell(x);
            if (c.width() == Cell::SpacerTail || c.width() == Cell::SpacerHead)
                continue;
            const char32_t cp = c.codepoint() ? c.codepoint() : U' ';
            line += QString::fromUcs4(&cp, 1);
            for (char32_t g : page->grapheme(x, y))
                line += QString::fromUcs4(&g, 1);
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        e.lines << line;
        e.wrapped << row.wrap;
    }
    return e;
}

std::optional<Snapshot> decodeFixture(const QString &name)
{
    const GhosttyFixture f = loadGhosttyFixture(fixtureDir, name);
    if (!f.ok) {
        QTextStream(stderr) << "FAIL: can't read " << name << "\n";
        ++failures;
        return std::nullopt;
    }
    Result r = decode(f.data);
    if (!r.snapshot) {
        QTextStream(stderr) << "FAIL: " << name << ": " << r.error << "\n";
        ++failures;
    }
    return r.snapshot;
}

// Replays a snapshot into a fresh emulator and checks it against the
// snapshot: lines, wrapping, cursor, the screen showing.
std::unique_ptr<Emulator> replayAndCompare(const Snapshot &snap, const ReplayOptions &options = {})
{
    auto emu = std::make_unique<Emulator>(snap.terminal.columns, snap.terminal.rows);
    emu->feed(replay(snap, options));

    const bool alternate = snap.terminal.activeScreen == 1 && snap.screen(1);
    CHECK(emu->showing() == emu->screen(alternate ? 1 : 0));

    if (const GhosttySnapshot::Screen *primary = snap.screen(0)) {
        QList<bool> wrapped;
        const QStringList got = screenLines(emu->screen(0), &wrapped);
        const Expected want = snapshotLines(snap, *primary, true);
        checkLines(got, want.lines, __LINE__);
        // The last row's wrap has nothing after it to show in.
        CHECK_EQ(wrapped.mid(0, wrapped.size() - 1), want.wrapped.mid(0, want.wrapped.size() - 1));
        if (!alternate) {
            // qtermwidget keeps a pending wrap as the cursor past the end.
            CHECK_EQ(emu->screen(0)->getCursorX(), int(primary->cursorX) + (primary->pendingWrap ? 1 : 0));
            CHECK_EQ(emu->screen(0)->getCursorY(), int(primary->cursorY));
        }
    }
    if (alternate) {
        const GhosttySnapshot::Screen &s = *snap.screen(1);
        const QStringList got = screenLines(emu->screen(1));
        checkLines(got.mid(got.size() - snap.terminal.rows), snapshotLines(snap, s, false).lines, __LINE__);
        CHECK_EQ(emu->screen(1)->getCursorX(), int(s.cursorX));
        CHECK_EQ(emu->screen(1)->getCursorY(), int(s.cursorY));
    }
    return emu;
}

// ---- Fixtures. -------------------------------------------------------------

void testFixtures()
{
    const QStringList complete = {
        QStringLiteral("complete-v1.hex"),        QStringLiteral("checkpoint-ready-v1.hex"),
        QStringLiteral("unknown-records-v1.hex"), QStringLiteral("kitty-alternate-v1.hex"),
        QStringLiteral("kitty-animation-v1.hex"), QStringLiteral("kitty-history-v1.hex"),
        QStringLiteral("kitty-loading-v1.hex"),   QStringLiteral("kitty-payload-in-history-v1.hex"),
        QStringLiteral("kitty-placed-v1.hex"),    QStringLiteral("kitty-relative-v1.hex"),
        QStringLiteral("kitty-source-rect-v1.hex"), QStringLiteral("kitty-virtual-v1.hex"),
    };
    for (const QString &name : complete) {
        current = name;
        const GhosttyFixture f = loadGhosttyFixture(fixtureDir, name);
        if (f.type != QLatin1String("ghostty_snapshot"))
            continue;  // checkpoint-ready is a record, not a whole snapshot
        const std::optional<Snapshot> snap = decodeFixture(name);
        if (!snap)
            continue;
        replayAndCompare(*snap);
        ReplayOptions plain;
        plain.kittyImages = false;
        replayAndCompare(*snap, plain);
        // Ending with the snapshot's half-parsed sequence.
        CHECK(replay(*snap).endsWith(snap->continuation));
    }
    current.clear();
}

void testComplete()
{
    current = QStringLiteral("complete-v1");
    const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("complete-v1.hex"));
    if (!snap)
        return;
    const QByteArray bytes = replay(*snap);
    CHECK(bytes.contains("\033]2;complete snapshot\033\\"));
    CHECK(bytes.contains("\033]7;file:///tmp/snapshot\033\\"));
    CHECK(bytes.contains("\033]4;7;rgb:01/02/03\033\\"));
    // Ghostty's test switches screens with no mode set: the old 47.
    CHECK(bytes.contains("\033[?47h"));

    auto emu = replayAndCompare(*snap);
    emu->settle();
    CHECK_EQ(emu->titles.value(2), QStringLiteral("complete snapshot"));
    // Back on the primary screen, its cursor where it was.
    emu->feed("\033[?47l");
    CHECK(emu->showing() == emu->screen(0));
    const GhosttySnapshot::Screen *primary = snap->screen(0);
    CHECK_EQ(emu->screen(0)->getCursorY(), int(primary->cursorY));
    CHECK_EQ(emu->screen(0)->getCursorX(), int(primary->cursorX));
    current.clear();
}

// Expected placement rows: from the screen's top, negative in the history.
void testKitty()
{
    {
        current = QStringLiteral("kitty-placed");
        const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("kitty-placed-v1.hex"));
        if (snap) {
            auto emu = replayAndCompare(*snap);
            const auto placements = emu->screen(0)->graphics().placements();
            CHECK_EQ(placements.size(), qsizetype(1));
            if (!placements.isEmpty()) {
                CHECK_EQ(placements[0].imageId, 1u);
                CHECK_EQ(placements[0].row, 0);
                CHECK_EQ(placements[0].column, 0);
                CHECK_EQ(placements[0].columns, 2);
                CHECK_EQ(placements[0].rows, 1);
            }
            const QImage img = emu->screen(0)->graphics().image(1);
            CHECK_EQ(img.size(), QSize(8, 8));
            CHECK_EQ(img.pixel(0, 0), qRgba(0xff, 0, 0, 0xff));
            // Images and their placements alone: the cursor stays put.
            CHECK_EQ(emu->screen(0)->getCursorX(), 0);
        }
    }
    {
        current = QStringLiteral("kitty-history");
        const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("kitty-history-v1.hex"));
        if (snap) {
            auto emu = replayAndCompare(*snap);
            // Rows: 4 of history then 2 on screen. e1 at history page row 1
            // col 1, e2 at row 0 of the newest history page, e3 on screen
            // row 1 of the first page. (e9 is relative, which qtermwidget
            // doesn't do.)
            const GhosttySnapshot::Screen &s = *snap->screen(0);
            QHash<quint32, std::pair<int, int>> want;  // placement id: row, column
            const int total = int(snapshotLines(*snap, s, true).lines.size());
            const int top = total - snap->terminal.rows;
            {
                // history pages newest first: index 0 is the newest
                int start = total - s.pageRows();
                for (const Page &p : s.history) {
                    start -= int(p.rowList.size());
                    for (const KittyPlacement &pl : p.kittyPlacements)
                        if (pl.location == KittyPlacement::Pin)
                            want[pl.key.id] = {start + pl.y - top, pl.x};
                }
                int pageStart = total - s.pageRows();
                QList<int> starts;
                for (const Page &p : s.pages) {
                    starts << pageStart;
                    pageStart += int(p.rowList.size());
                }
                for (const KittyPlacement &pl : s.kitty->placements)
                    if (pl.location == KittyPlacement::Pin)
                        want[pl.key.id] = {starts[pl.page] + pl.y - top, pl.x};
            }
            CHECK_EQ(want.size(), qsizetype(3));
            const auto placements = emu->screen(0)->graphics().placements();
            CHECK_EQ(placements.size(), qsizetype(3));
            for (const auto &p : placements) {
                CHECK(want.contains(p.placementId));
                CHECK_EQ(p.row, want.value(p.placementId).first);
                CHECK_EQ(p.column, want.value(p.placementId).second);
            }
        }
    }
    {
        current = QStringLiteral("kitty-virtual");
        const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("kitty-virtual-v1.hex"));
        if (snap) {
            auto emu = replayAndCompare(*snap);
            const auto placements = emu->screen(0)->graphics().placements();
            bool anyVirtual = false;
            for (const auto &p : placements)
                anyVirtual = anyVirtual || p.isVirtual;
            CHECK(anyVirtual);
        }
    }
    {
        current = QStringLiteral("kitty-alternate");
        const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("kitty-alternate-v1.hex"));
        if (snap) {
            auto emu = replayAndCompare(*snap);
            // Each screen's images on its own screen.
            for (quint16 key : {quint16(0), quint16(1)}) {
                const GhosttySnapshot::Screen *s = snap->screen(key);
                if (!s || !s->kitty)
                    continue;
                if (key == 1 && snap->terminal.activeScreen != 1)
                    continue;  // an alternate screen not showing isn't replayed
                for (const KittyImage &img : s->kitty->images)
                    if (!img.data.isEmpty())
                        CHECK(!emu->screen(key)->graphics().image(img.id).isNull());
            }
        }
    }
    {
        current = QStringLiteral("kitty-animation");
        const std::optional<Snapshot> snap = decodeFixture(QStringLiteral("kitty-animation-v1.hex"));
        if (snap) {
            const QByteArray bytes = replay(*snap);
            const KittyImage *animated = nullptr;
            for (const KittyImage &img : snap->screen(0)->kitty->images)
                if (img.animated)
                    animated = &img;
            CHECK(animated);
            if (animated) {
                // v=3 was asked for; Ghostty keeps 2.
                CHECK(bytes.contains(",v=3,"));
                CHECK_EQ(qsizetype(bytes.count("a=f,")), animated->frames.size());
            }
            replayAndCompare(*snap);
        }
    }
    current.clear();
}

// ---- A snapshot made here: what the fixtures don't have. ---------------------

quint64 cellWord(char32_t cp, quint16 style = 0, Cell::Width width = Cell::Narrow, quint16 link = 0)
{
    return quint64(cp & 0xFFFFFF) << 2 | quint64(style) << 26 | quint64(width) << 42 | quint64(link) << 48;
}

Row textRow(const QString &text, bool wrap = false)
{
    Row r;
    r.wrap = wrap;
    for (char32_t cp : text.toUcs4())
        r.cells.append(Cell{cellWord(cp == U' ' ? 0 : cp)});
    return r;
}

Snapshot made(quint16 columns, quint16 rows)
{
    Snapshot snap;
    TerminalState &t = snap.terminal;
    t.columns = columns;
    t.rows = rows;
    t.scrollBottom = quint16(rows - 1);
    t.scrollRight = quint16(columns - 1);
    t.currentModes.bits = (1ull << ModeWraparound) | (1ull << ModeCursorVisible) | (1ull << ModeSendReceive);
    for (int x = 0; x < columns; ++x)
        t.tabStops.append(x % 8 == 0);
    GhosttySnapshot::Screen s;
    s.key = 0;
    snap.screens.append(s);
    return snap;
}

void testMade()
{
    current = QStringLiteral("made");
    {
        // History across two pages, wrapped lines, wide characters (one
        // wrapping early), a grapheme cluster, background-only cells.
        Snapshot snap = made(6, 3);
        GhosttySnapshot::Screen &s = snap.screens[0];

        Page older;
        older.columns = 6;
        older.rows = 2;
        older.rowList << textRow(QStringLiteral("abcdef"), true) << textRow(QStringLiteral("gh"));
        Page newer;
        newer.columns = 6;
        newer.rows = 1;
        newer.rowList << textRow(QStringLiteral("one"));
        s.history << newer << older;  // newest first

        Page p;
        p.columns = 6;
        p.rows = 4;
        Style red;
        red.foreground = Color{Color::Palette, 1, 0, 0, 0};
        red.flags = Style::Bold;
        p.styles.insert(1, red);
        Row wide;
        wide.wrap = true;
        wide.cells << Cell{cellWord(U'x', 1)} << Cell{cellWord(U'中', 0, Cell::Wide)}
                   << Cell{cellWord(0, 0, Cell::SpacerTail)} << Cell{cellWord(U'y')}
                   << Cell{cellWord(U'z')} << Cell{cellWord(0, 0, Cell::SpacerHead)};
        Row next;
        next.cells << Cell{cellWord(U'文', 0, Cell::Wide)} << Cell{cellWord(0, 0, Cell::SpacerTail)}
                   << Cell{cellWord(U'e')};
        p.graphemes.insert(Page::graphemeKey(2, 2), {U'́'});  // é
        Row bg;
        bg.cells << Cell{quint64(Cell::PaletteBackground) | quint64(4) << 2} << Cell{cellWord(U'q')};
        p.rowList << textRow(QStringLiteral("hist")) << wide << next << bg;
        s.pages << p;
        s.cursorX = 3;
        s.cursorY = 2;

        auto emu = replayAndCompare(snap);
        // The style came through on 'x'; the background-only cell is blue.
        const Konsole::Screen *scr = emu->screen(0);
        const int total = scr->getHistLines() + scr->getLines();
        QList<Konsole::Character> image(total * 6);
        scr->getImage(image.data(), int(image.size()), 0, total - 1);
        const int xRow = total - 3;  // the "x中yz" row is the screen's first
        CHECK(image[xRow * 6].rendition & RE_BOLD);
        // qtermwidget keeps a bold base colour as its bright one.
        Konsole::CharacterColor brightRed(COLOR_SPACE_SYSTEM, 1);
        brightRed.setIntensive();
        CHECK(image[xRow * 6].foregroundColor == brightRed);
        CHECK(image[(total - 1) * 6].backgroundColor == Konsole::CharacterColor(COLOR_SPACE_SYSTEM, 4));
    }
    {
        // A character Ghostty made wide (an emoji presentation selector)
        // that the target may not: what follows stays in Ghostty's column.
        Snapshot snap = made(6, 1);
        GhosttySnapshot::Screen &s = snap.screens[0];
        Page p;
        p.columns = 6;
        p.rows = 1;
        Row r;
        r.cells << Cell{cellWord(U'❤', 0, Cell::Wide)} << Cell{cellWord(0, 0, Cell::SpacerTail)}
                << Cell{cellWord(U'a')};
        p.graphemes.insert(Page::graphemeKey(0, 0), {U'️'});
        p.rowList << r;
        s.pages << p;
        Emulator emu(6, 1);
        emu.feed(replay(snap));
        QList<Konsole::Character> image(6);
        emu.screen(0)->getImage(image.data(), 6, 0, 0);
        CHECK(image[2].character == U'a');
    }
    {
        // The cursor waiting to wrap: the next character goes on a new line.
        Snapshot snap = made(4, 2);
        GhosttySnapshot::Screen &s = snap.screens[0];
        Page p;
        p.columns = 4;
        p.rows = 2;
        p.rowList << textRow(QStringLiteral("abcd")) << textRow(QString());
        s.pages << p;
        s.cursorX = 3;
        s.cursorY = 0;
        s.pendingWrap = true;
        auto emu = replayAndCompare(snap);
        emu->feed("Z");
        checkLines(screenLines(emu->screen(0)), {QStringLiteral("abcd"), QStringLiteral("Z")}, __LINE__);
    }
    {
        // Scroll region, origin mode, the pen, a saved cursor, tab stops.
        Snapshot snap = made(10, 5);
        TerminalState &t = snap.terminal;
        t.scrollTop = 1;
        t.scrollBottom = 3;
        t.currentModes.bits |= 1ull << ModeOrigin | 1ull << ModeBracketedPaste;
        t.tabStops = QList<bool>(10, false);
        t.tabStops[3] = true;
        GhosttySnapshot::Screen &s = snap.screens[0];
        Page p;
        p.columns = 10;
        p.rows = 5;
        for (int i = 0; i < 5; ++i)
            p.rowList << textRow(QString::number(i));
        s.pages << p;
        s.cursorX = 2;
        s.cursorY = 2;
        s.pen.flags = Style::Italic;
        SavedCursor saved;
        saved.x = 5;
        saved.y = 4;
        s.savedCursor = saved;
        auto emu = replayAndCompare(snap);
        // Origin mode: row 1 of the region is screen row 1 (0-based).
        emu->feed("\033[1;1H");
        CHECK_EQ(emu->screen(0)->getCursorY(), 1);
        // The region: a line feed at its bottom scrolls only it.
        emu->feed("\033[3;1H\nN");
        checkLines(screenLines(emu->screen(0)),
                   {QStringLiteral("0"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("N"),
                    QStringLiteral("4")},
                   __LINE__);
        // Tab stops: from column 0, the next is 3.
        emu->feed("\033[?6l\033[5;1H\t");
        CHECK_EQ(emu->screen(0)->getCursorX(), 3);
        // The saved cursor comes back.
        emu->feed("\0338");
        CHECK_EQ(emu->screen(0)->getCursorX(), 5);
        CHECK_EQ(emu->screen(0)->getCursorY(), 4);
        const QByteArray bytes = replay(snap);
        CHECK(bytes.contains("2004"));
    }
    current.clear();
}

void testPieces()
{
    using namespace GhosttySnapshot::Replay;
    Style s;
    CHECK_EQ(sgr(s, false), QByteArray("\033[0m"));
    s.flags = Style::Bold | Style::Inverse;
    s.underline = Style::Curly;
    s.foreground = Color{Color::Palette, 9, 0, 0, 0};
    s.background = Color{Color::Rgb, 0, 1, 2, 3};
    s.underlineColor = Color{Color::Palette, 200, 0, 0, 0};
    CHECK_EQ(sgr(s, false), QByteArray("\033[0;1;4;7;91;48;2;1;2;3m"));
    CHECK_EQ(sgr(s, true), QByteArray("\033[0;1;4:3;7;91;48;2;1;2;3;58:5:200m"));
    s = Style();
    s.foreground = Color{Color::Palette, 100, 0, 0, 0};
    s.background = Color{Color::Palette, 3, 0, 0, 0};
    CHECK_EQ(sgr(s, false), QByteArray("\033[0;38;5;100;43m"));

    CHECK_EQ(colorSpec(Rgb{0xAB, 0x0C, 0xFF}), QByteArray("rgb:ab/0c/ff"));

    // Chunks of at most 4096 base64 bytes, the keys on the first only.
    const QByteArray data(5000, 'x');
    const QByteArray t = kittyTransmit("a=t,i=1", data);
    CHECK_EQ(qsizetype(t.count("\033_G")), qsizetype(2));
    CHECK(t.startsWith("\033_Ga=t,i=1,m=1;"));
    CHECK(t.contains("\033_Gq=2,m=0;"));
    QByteArray b64;
    for (const QByteArray &chunk : t.split('\033')) {
        const qsizetype semi = chunk.indexOf(';');
        if (chunk.startsWith("_G") && semi > 0)
            b64 += chunk.mid(semi + 1);
    }
    CHECK_EQ(QByteArray::fromBase64(b64), data);
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    fixtureDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(PJ_GHOSTTY_FIXTURES);

    testPieces();
    testFixtures();
    testComplete();
    testKitty();
    testMade();

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
