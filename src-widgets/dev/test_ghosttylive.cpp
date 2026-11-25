/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-ghosttylive: snapshots from a real Ghostty terminal (libghostty-vt),
 * checked twice over. Each byte stream goes
 *
 *   into Ghostty -> snapshot -> our decoder -> compared with what Ghostty
 *                                              says of its own state
 *                           -> replay -> qtermwidget (A)
 *   straight into qtermwidget (B)
 *
 * and A must match B cell for cell (characters, attributes, colours), with
 * the same cursor, screen, wrapped lines, images and title; and again after
 * more bytes to both. A difference is a fault in the decoder or the replay,
 * or a place where Ghostty and qtermwidget read the bytes differently; the
 * curated streams keep clear of those found:
 *
 *   - qtermwidget's ED 2 moves the screen into the history.
 *   - qtermwidget gives each screen its own cursor and pen; Ghostty, as
 *     xterm, takes them to the alternate screen.
 *   - qtermwidget keeps a pending wrap as the cursor past the last column,
 *     so a line feed then keeps it there (the next character wraps; Ghostty
 *     writes it in the last column, as xterm) and ED/EL from there spare
 *     the last character.
 *   - Erasing or writing over half a wide character: Ghostty clears the
 *     other half, qtermwidget leaves it.
 *   - After erasing, Ghostty clears a line's wrapped flag; qtermwidget
 *     keeps it.
 *
 * Random streams meet all of those, so the replay is checked against
 * Ghostty itself there: A's lines, wrapped lines, cursor and screen against
 * the snapshot, then after more bytes against a new snapshot.
 *
 * Run: ./build/test-ghosttylive [random streams, default 300] [-v]
 *      ./build/test-ghosttylive --reduce SEED: the smallest part of a
 *      failing random stream that still fails
 *      ./build/test-ghosttylive --file CAPTURE COLUMNS ROWS: a stream
 *      captured from a program (script -q -c '...' /dev/null > CAPTURE)
 */

#include "ghosttyreplay.h"
#include "ghosttysnapshot.h"
#include "ghosttyvt.h"
#include "qtermprobe.h"

#include <QApplication>
#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>

using namespace GhosttySnapshot;
using QtermProbeLines::screenCells;
using QtermProbeLines::screenLines;

namespace {

int failures = 0;
int checks = 0;
QString current;
bool verbose = false;
bool quiet = false;  // while reducing

bool check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        if (!quiet)
            QTextStream(stderr) << "FAIL [" << current << "]: " << what << "\n";
        ++failures;
    }
    return ok;
}

QString show(const QStringList &lines)
{
    QString out;
    for (const QString &l : lines)
        out += QStringLiteral("      |") + l + QStringLiteral("|\n");
    return out;
}

QString cellText(const Konsole::Character &c)
{
    return QStringLiteral("U+%1 r%2 fg%3 bg%4")
        .arg(quint32(c.character), 4, 16, QLatin1Char('0'))
        .arg(c.rendition)
        .arg(c.foregroundColor.value(), 0, 16)
        .arg(c.backgroundColor.value(), 0, 16);
}

// Whether two cells look the same. On a blank, the foreground, bold and
// italic don't show (qtermwidget leaves the pen's on erased cells, Ghostty
// only the background) unless something is drawn with them.
bool sameLook(const Konsole::Character &p, const Konsole::Character &q)
{
    if (p.character != q.character || p.backgroundColor != q.backgroundColor)
        return false;
    const bool blank = p.character == U' ' || p.character == 0;
    constexpr int drawn = RE_REVERSE | RE_UNDERLINE | RE_STRIKEOUT | RE_OVERLINE;
    if (blank && !(p.rendition & drawn) && !(q.rendition & drawn))
        return (p.rendition & RE_CONCEAL) == (q.rendition & RE_CONCEAL);
    return p.rendition == q.rendition && p.foregroundColor == q.foregroundColor;
}

struct Case
{
    QString name;
    int columns = 20;
    int rows = 5;
    QByteArray stream;
    QByteArray after;  // fed to both afterwards: carries on from a cut
    QList<QByteArray> pieces;  // random streams: what `stream` was made of
    // Random streams are checked against Ghostty, not against qtermwidget
    // fed directly: the two differ on too much of what random bytes reach
    // (see the top of the file).
    bool random = false;
};

// Our decoder against Ghostty's own view of its state.
void checkDecoder(const GhosttyVt &vt, const Snapshot &snap)
{
    check(snap.terminal.activeScreen == vt.activeScreen(), QStringLiteral("active screen"));
    const GhosttySnapshot::Screen *s = snap.screen(snap.terminal.activeScreen);
    if (!check(s, QStringLiteral("the active screen is there")))
        return;
    check(s->cursorX == vt.cursorX() && s->cursorY == vt.cursorY(),
          QStringLiteral("cursor %1,%2, Ghostty says %3,%4").arg(s->cursorX).arg(s->cursorY).arg(vt.cursorX()).arg(vt.cursorY()));
    check(s->pendingWrap == vt.pendingWrap(), QStringLiteral("pending wrap"));
    check(snap.terminal.currentModes.has(ModeCursorVisible) == vt.cursorVisible(), QStringLiteral("cursor visible"));
    check(snap.terminal.title == vt.title(), QStringLiteral("title"));
    check(snap.terminal.pwd == vt.pwd(), QStringLiteral("pwd"));

    // The screen as text, history and all, as Ghostty's formatter gives it.
    QList<const Page *> pages;
    for (auto it = s->history.crbegin(); it != s->history.crend(); ++it)
        pages << &*it;
    for (const Page &p : s->pages)
        pages << &p;
    QList<QString> rows;
    for (const Page *pp : pages)
        for (int y = 0; y < pp->rowList.size(); ++y) {
            const Page &p = *pp;
            const Row &row = p.rowList[y];
            QString line;
            for (int x = 0; x < snap.terminal.columns; ++x) {
                const Cell c = row.cell(x);
                if (c.width() == Cell::SpacerTail || c.width() == Cell::SpacerHead)
                    continue;
                const char32_t cp = c.codepoint() ? c.codepoint() : U' ';
                line += QString::fromUcs4(&cp, 1);
                for (char32_t g : p.grapheme(x, y))
                    line += QString::fromUcs4(&g, 1);
            }
            while (line.endsWith(QLatin1Char(' ')))
                line.chop(1);
            rows << line;
        }
    QStringList active = rows;
    while (!active.isEmpty() && active.last().isEmpty())
        active.removeLast();
    const QString ours = active.join(QLatin1Char('\n'));
    // The formatter keeps blank lines down to the cursor.
    QString theirs = QString::fromUtf8(vt.plainText());
    while (theirs.endsWith(QLatin1Char('\n')))
        theirs.chop(1);
    check(ours == theirs, QStringLiteral("screen text\n    ours:\n%1    Ghostty's:\n%2")
                              .arg(show(active), show(theirs.split(QLatin1Char('\n')))));
}

// A (replayed) against B (fed directly).
void compareEmulators(QtermProbe &a, QtermProbe &b)
{
    const int showing = b.showing() == b.screen(1) ? 1 : 0;
    check((a.showing() == a.screen(1) ? 1 : 0) == showing, QStringLiteral("screen showing"));

    for (int i = 0; i < 2; ++i) {
        // An alternate screen not showing isn't replayed.
        if (i == 1 && showing == 0)
            continue;
        const Konsole::Screen *sa = a.screen(i), *sb = b.screen(i);
        QList<bool> wa, wb;
        QStringList la = screenLines(sa, &wa), lb = screenLines(sb, &wb);
        // The alternate screen has no history to speak of.
        int skipA = 0, skipB = 0;
        if (i == 1) {
            skipA = int(la.size()) - sa->getLines();
            skipB = int(lb.size()) - sb->getLines();
            la = la.mid(skipA);
            lb = lb.mid(skipB);
            wa = wa.mid(skipA);
            wb = wb.mid(skipB);
        }
        if (!check(la == lb, QStringLiteral("screen %1 lines\n    replayed:\n%2    direct:\n%3").arg(i).arg(show(la), show(lb))))
            continue;
        if (!wa.isEmpty() && !wb.isEmpty())
            check(wa.mid(0, wa.size() - 1) == wb.mid(0, wb.size() - 1), QStringLiteral("screen %1 wrapped lines").arg(i));

        const QList<Konsole::Character> ca = screenCells(sa), cb = screenCells(sb);
        const int columns = sa->getColumns();
        int reported = 0;
        for (int y = 0; y < la.size() && reported < 3; ++y) {
            for (int x = 0; x < columns && reported < 3; ++x) {
                const Konsole::Character &p = ca[(y + skipA) * columns + x];
                const Konsole::Character &q = cb[(y + skipB) * columns + x];
                if (!sameLook(p, q)) {
                    check(false, QStringLiteral("screen %1 cell %2,%3: replayed %4, direct %5")
                                     .arg(i).arg(x).arg(y).arg(cellText(p), cellText(q)));
                    ++reported;
                }
            }
        }
        if (!reported)
            check(true, QString());
    }
    for (int i = 0; i < 2; ++i) {
        if (i == 1 && showing == 0)
            continue;
        auto key = [](const Konsole::GraphicsPlacement &p) {
            return QStringLiteral("image %1 at %2,%3 %4x%5 z%6%7")
                .arg(p.imageId).arg(p.column).arg(p.row).arg(p.columns).arg(p.rows).arg(p.z)
                .arg(p.isVirtual ? QStringLiteral(" virtual") : QString());
        };
        QStringList pa, pb;
        for (const auto &p : a.screen(i)->graphics().placements())
            pa << key(p);
        for (const auto &p : b.screen(i)->graphics().placements())
            pb << key(p);
        pa.sort();
        pb.sort();
        check(pa == pb, QStringLiteral("screen %1 images: replayed [%2], direct [%3]")
                            .arg(i).arg(pa.join(QStringLiteral("; ")), pb.join(QStringLiteral("; "))));
    }
    const Konsole::Screen *sa = a.showing(), *sb = b.showing();
    check(sa->getCursorX() == sb->getCursorX() && sa->getCursorY() == sb->getCursorY(),
          QStringLiteral("cursor: replayed %1,%2, direct %3,%4")
              .arg(sa->getCursorX()).arg(sa->getCursorY()).arg(sb->getCursorX()).arg(sb->getCursorY()));
}

// What the replay left in qtermwidget against the snapshot it came from:
// every line, wrapped lines, the screen showing and the cursor.
void compareToSnapshot(QtermProbe &a, const Snapshot &snap)
{
    auto rowsOf = [&](const GhosttySnapshot::Screen &s, bool withHistory) {
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
        QStringList lines;
        QList<bool> wrapped;
        for (const auto &[page, y] : rows) {
            const Row &row = page->rowList[y];
            QString line;
            for (int x = 0; x < snap.terminal.columns; ++x) {
                const Cell cell = row.cell(x);
                if (cell.width() == Cell::SpacerTail || cell.width() == Cell::SpacerHead)
                    continue;
                const char32_t cp = cell.codepoint() ? cell.codepoint() : U' ';
                line += QString::fromUcs4(&cp, 1);
                for (char32_t g : page->grapheme(x, y))
                    line += QString::fromUcs4(&g, 1);
            }
            while (line.endsWith(QLatin1Char(' ')))
                line.chop(1);
            lines << line;
            wrapped << row.wrap;
        }
        return std::make_pair(lines, wrapped);
    };

    const bool alternate = snap.terminal.activeScreen == 1 && snap.screen(1);
    check(a.showing() == a.screen(alternate ? 1 : 0), QStringLiteral("screen showing"));
    for (int i = 0; i < (alternate ? 2 : 1); ++i) {
        const GhosttySnapshot::Screen *s = snap.screen(quint16(i));
        if (!s)
            continue;
        const auto [want, wantWrapped] = rowsOf(*s, i == 0);
        QList<bool> gotWrapped;
        QStringList got = screenLines(a.screen(i), &gotWrapped);
        if (i == 1) {
            const qsizetype skip = got.size() - a.screen(1)->getLines();
            got = got.mid(skip);
            gotWrapped = gotWrapped.mid(skip);
        }
        if (!check(got == want, QStringLiteral("screen %1 lines\n    replayed:\n%2    Ghostty:\n%3").arg(i).arg(show(got), show(want))))
            continue;
        check(gotWrapped.mid(0, gotWrapped.size() - 1) == wantWrapped.mid(0, wantWrapped.size() - 1),
              QStringLiteral("screen %1 wrapped lines").arg(i));
    }
    const GhosttySnapshot::Screen *s = snap.screen(alternate ? 1 : 0);
    if (s) {
        // qtermwidget keeps a pending wrap as the cursor past the end.
        const int x = s->cursorX + (s->pendingWrap ? 1 : 0);
        check(a.showing()->getCursorX() == x && a.showing()->getCursorY() == s->cursorY,
              QStringLiteral("cursor: replayed %1,%2, Ghostty %3,%4")
                  .arg(a.showing()->getCursorX()).arg(a.showing()->getCursorY()).arg(x).arg(s->cursorY));
    }
}

void run(const Case &c)
{
    current = c.name;
    GhosttyVt vt{quint16(c.columns), quint16(c.rows)};
    if (!check(vt.ok(), QStringLiteral("a Ghostty terminal")))
        return;
    vt.write(c.stream);
    const QByteArray bytes = vt.snapshot();
    if (!check(!bytes.isEmpty(), QStringLiteral("Ghostty encodes a snapshot")))
        return;
    check(scan(bytes) == bytes.size(), QStringLiteral("scan finds its end"));
    Result r = decode(bytes);
    if (!check(bool(r.snapshot), QStringLiteral("decodes: ") + r.error))
        return;
    checkDecoder(vt, *r.snapshot);

    QtermProbe a(c.columns, c.rows), b(c.columns, c.rows);
    const QByteArray replayed = replay(*r.snapshot);
    if (verbose)
        QTextStream(stdout) << c.name << ": " << c.stream.size() << " bytes -> snapshot " << bytes.size()
                            << " -> replay " << replayed.size() << "\n";
    a.feed(replayed);
    if (c.random) {
        compareToSnapshot(a, *r.snapshot);
        if (!c.after.isEmpty()) {
            // Carrying on: the same bytes to both, then Ghostty's word again.
            current = c.name + QStringLiteral(" (after)");
            vt.write(c.after);
            const Result again = decode(vt.snapshot());
            a.feed(c.after);
            if (check(bool(again.snapshot), QStringLiteral("decodes: ") + again.error))
                compareToSnapshot(a, *again.snapshot);
        }
        current.clear();
        return;
    }
    b.feed(c.stream);
    compareEmulators(a, b);
    a.settle();
    b.settle();
    check(a.titles.value(2) == b.titles.value(2) || a.titles.value(0) == b.titles.value(0),
          QStringLiteral("title: replayed '%1', direct '%2'").arg(a.titles.value(2), b.titles.value(2)));

    if (!c.after.isEmpty()) {
        current = c.name + QStringLiteral(" (after)");
        a.feed(c.after);
        b.feed(c.after);
        compareEmulators(a, b);
    }
    current.clear();
}

QByteArray lines(int from, int to, const char *prefix = "line ")
{
    QByteArray out;
    for (int i = from; i <= to; ++i)
        out += prefix + QByteArray::number(i) + "\r\n";
    return out;
}

Case curatedCase(const QString &name, int columns, int rows, const QByteArray &stream, const QByteArray &after)
{
    Case c;
    c.name = name;
    c.columns = columns;
    c.rows = rows;
    c.stream = stream;
    c.after = after;
    return c;
}

QList<Case> curated()
{
    QList<Case> cs;
    cs << curatedCase(QStringLiteral("empty"), 20, 5, "", "hello");
    cs << curatedCase(QStringLiteral("plain"), 20, 5, lines(1, 3), "more");
    cs << curatedCase(QStringLiteral("history"), 20, 5, lines(1, 40), "next\r\n");
    cs << curatedCase(QStringLiteral("deep history"), 30, 6, lines(1, 3000, "a longer line of history "), {});
    cs << curatedCase(QStringLiteral("wrapping"), 10, 4,
               "0123456789abcdefghijklmnopqrstuvwxyz\r\nexactly10c\r\nshort\r\n" + QByteArray(57, 'w') + "\r\nend",
               "ABC");
    cs << curatedCase(QStringLiteral("full last column"), 10, 3, "first\r\n0123456789", "X");
    cs << curatedCase(QStringLiteral("styles"), 30, 6,
               "\033[1mbold\033[0m \033[2mfaint\033[0m \033[3mitalic\033[0m \033[4munder\033[0m\r\n"
               "\033[5mblink\033[0m \033[7mreverse\033[0m \033[8mhidden\033[0m \033[9mstrike\033[0m\r\n"
               "\033[31mred\033[42mgreen bg\033[0m \033[91mbright\033[0m \033[1;34mboldblue\033[0m\r\n"
               "\033[38;5;100m256fg\033[48;5;200m256bg\033[0m \033[38;2;10;20;30mrgb\033[48;2;1;2;3mrgbbg\033[0m\r\n"
               "\033[1;3;4;31;47mall at once\033[0m plain",
               "\033[32mafter");
    cs << curatedCase(QStringLiteral("pen carries on"), 20, 4, "\033[1;33;44mstill styled", " more");
    cs << curatedCase(QStringLiteral("wide"), 12, 4, "中文字符 and ascii\r\nab中文cd\r\n0123456789中文\r\n", "末");
    cs << curatedCase(QStringLiteral("wide wraps early"), 5, 4, "abcd中ef", "g");
    cs << curatedCase(QStringLiteral("combining"), 20, 3, "cafe\u0301 nai\u0308ve\r\n", "e\u0301");
    cs << curatedCase(QStringLiteral("erase"), 20, 5,
               "aaaaaaaaaaaaaaaaaaaa\r\nbbbbbbbbbbbbbbbbbbbb\r\ncccccccccccccccccccc\r\n"
               "\033[2;5H\033[K\033[3;5H\033[1K\033[1;10H\033[0J",
               "Z");
    cs << curatedCase(QStringLiteral("erase with colour"), 20, 4, "abc\033[44m\033[K\r\nde\033[45m\033[1K\033[0m\r\n\033[42m\033[0Jdone", "!");
    cs << curatedCase(QStringLiteral("cursor moves"), 20, 6,
               "\033[3;7Hx\033[1;1Hy\033[6;20Hz\033[2;2H\033[3Cw\033[A\033[2Dv", "u");
    cs << curatedCase(QStringLiteral("alternate screen"), 20, 5,
               lines(1, 12) + "\033[?1049h\033[H\033[2Jtop line\033[3;4Hmiddle\033[5;1Hbottom\033[2;2H",
               "\033[?1049lback");
    cs << curatedCase(QStringLiteral("alternate 1047"), 20, 4, "shell\r\n\033[?1047h\033[H\033[2Jfull screen\033[2;3H", "\033[?1047lshell again");
    cs << curatedCase(QStringLiteral("alternate and back"), 20, 4, "before\r\n\033[?1049hgone\033[?1049lafter", "!");
    cs << curatedCase(QStringLiteral("scroll region"), 20, 6,
               "1\r\n2\r\n3\r\n4\r\n5\r\n6\033[2;4r\033[4;1Hfoo\nbar\nbaz\n",
               "\nqux");
    cs << curatedCase(QStringLiteral("origin mode"), 20, 6, "\033[2;5r\033[?6h\033[1;1Horigin\033[2;3Hx", "\033[1;1Hy");
    cs << curatedCase(QStringLiteral("tab stops"), 30, 3, "\033[3g\033[1;5H\033H\033[1;12H\033H\033[1;1Ha\tb\tc\r\n", "x\ty\tz");
    cs << curatedCase(QStringLiteral("saved cursor"), 20, 4, "\033[2;6H\033[1;31m\0337\033[0m\033[4;1Hbottom", "\0338saved");
    cs << curatedCase(QStringLiteral("title and pwd"), 20, 3, "\033]2;my title\007\033]7;file:///tmp/here\033\\text", {});
    cs << curatedCase(QStringLiteral("hyperlinks"), 30, 3,
               "see \033]8;;https://example.com\033\\a link\033]8;;\033\\ and \033]8;id=x;https://e.org\033\\another\033]8;;\033\\",
               {});
    cs << curatedCase(QStringLiteral("dec graphics"), 20, 3, "\033(0lqqk\r\nx  x\r\nmqqj\033(B ok", "\033(0q");
    cs << curatedCase(QStringLiteral("shifted charset"), 20, 3, "\033)0a\016lqk\017b", "\016q");
    cs << curatedCase(QStringLiteral("insert mode"), 20, 3, "abcdef\033[1;3H\033[4hXY", "Z");
    cs << curatedCase(QStringLiteral("no autowrap"), 10, 3, "\033[?7l0123456789abc", "d");
    cs << curatedCase(QStringLiteral("hidden cursor"), 20, 3, "\033[?25lhidden", "\033[?25h");
    cs << curatedCase(QStringLiteral("cut in SGR"), 20, 3, "plain \033[3", "1mred");
    cs << curatedCase(QStringLiteral("cut in OSC"), 20, 3, "text \033]2;half a ti", "tle\007more");
    cs << curatedCase(QStringLiteral("cut in UTF-8"), 20, 3, "caf\xc3", "\xa9 ok");
    cs << curatedCase(QStringLiteral("cut after ESC"), 20, 3, "abc\033", "[1mbold");
    cs << curatedCase(QStringLiteral("kitty image"), 20, 5,
               "img:\033_Ga=T,f=32,s=2,v=2,i=7,q=2;" + QByteArray(16, '\x7f').toBase64() + "\033\\ after",
               "!");
    cs << curatedCase(QStringLiteral("long session"), 80, 24, {}, {});
    {
        // Something like a build log: colours, progress lines, wrapping.
        QByteArray s;
        for (int i = 0; i < 400; ++i) {
            s += "\033[32m[" + QByteArray::number(i) + "/400]\033[0m Compiling src/module_" + QByteArray::number(i)
                + ".cpp";
            if (i % 7 == 0)
                s += " \033[33mwarning:\033[0m " + QByteArray(100, 'x');
            s += "\r\n";
        }
        cs.last().stream = s;
        cs.last().after = "\033[1mdone\033[0m";
    }
    return cs;
}

// A random stream: text (wide characters too), line breaks, moves, styles,
// erasing, tabs, screens and saved cursors.
Case randomCase(quint32 seed)
{
    QRandomGenerator rng(seed);
    Case c;
    c.name = QStringLiteral("random %1").arg(seed);
    c.columns = 8 + int(rng.bounded(30));
    c.rows = 3 + int(rng.bounded(10));
    const QList<QByteArray> words = {"a", "word", "longer-word", "x", "  ", "e\u0301", "0123456789",
                                     "\u2500\u2502", "!@#$%", "中", "文字", "中文字符"};
    const int pieces = 20 + int(rng.bounded(300));
    for (int i = 0; i < pieces; ++i) {
        const qsizetype start = c.stream.size();
        switch (rng.bounded(14)) {
        case 0:
        case 1:
        case 2:
        case 3: c.stream += words[int(rng.bounded(int(words.size())))]; break;
        case 4: c.stream += "\r\n"; break;
        case 5: c.stream += "\n"; break;
        case 6: c.stream += "\r"; break;
        case 7:
            c.stream += "\033[" + QByteArray::number(1 + rng.bounded(c.rows)) + ';'
                + QByteArray::number(1 + rng.bounded(c.columns)) + 'H';
            break;
        case 8: {
            static const char *sgr[] = {"0", "1", "3", "4", "7", "31", "42", "93", "104", "38;5;123",
                                        "48;2;9;8;7", "22", "24", "27", "39", "49"};
            c.stream += QByteArray("\033[") + sgr[rng.bounded(int(std::size(sgr)))] + 'm';
            break;
        }
        case 9: c.stream += QByteArray("\033[") + "012"[rng.bounded(3)] + 'K'; break;
        case 10: c.stream += QByteArray("\033[") + "012"[rng.bounded(3)] + 'J'; break;
        case 11: c.stream += QByteArray("\033[") + QByteArray::number(1 + rng.bounded(4)) + "ABCD"[rng.bounded(4)]; break;
        case 12: c.stream += "\t"; break;
        case 13:
            if (rng.bounded(4) == 0)
                c.stream += rng.bounded(2) ? "\033[?1049h" : "\033[?1049l";
            else
                c.stream += rng.bounded(2) ? "\0337" : "\0338";
            break;
        }
        c.pieces << c.stream.mid(start);
    }
    // Moving on only: writing would meet the wide-character difference.
    c.after = "\033[0m\r\n\n";
    c.random = true;
    return c;
}

QByteArray escaped(const QByteArray &bytes)
{
    QByteArray out;
    for (char ch : bytes) {
        const auto u = quint8(ch);
        if (u == 0x1b)
            out += "\\e";
        else if (u == '\r')
            out += "\\r";
        else if (u == '\n')
            out += "\\n";
        else if (u < 0x20 || u == 0x7f)
            out += "\\x" + QByteArray::number(u, 16).rightJustified(2, '0');
        else
            out += ch;
    }
    return out;
}

bool fails(const Case &c)
{
    const int before = failures;
    quiet = true;
    run(c);
    quiet = false;
    const bool failed = failures > before;
    failures = before;
    return failed;
}

// The fewest pieces of a failing random stream that still fail, shown and
// then run again aloud.
void reduce(Case c)
{
    if (!fails(c)) {
        QTextStream(stdout) << c.name << " passes\n";
        return;
    }
    for (qsizetype chunk = c.pieces.size() / 2; chunk >= 1; chunk /= 2) {
        for (qsizetype i = 0; i + chunk <= c.pieces.size();) {
            Case t = c;
            t.pieces.remove(i, chunk);
            t.stream = t.pieces.join();
            if (fails(t))
                c = t;
            else
                i += chunk;
        }
    }
    QTextStream(stdout) << c.name << " reduced to " << c.columns << "x" << c.rows << ": \""
                        << escaped(c.stream) << "\"\n";
    run(c);
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    int seeds = 300;
    QList<int> toReduce;
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "-v")
            verbose = true;
        else if (QByteArray(argv[i]) == "--reduce" && i + 1 < argc)
            toReduce << QByteArray(argv[++i]).toInt();
        else if (QByteArray(argv[i]) == "--file" && i + 3 < argc) {
            // A captured stream, checked against both: Ghostty and
            // qtermwidget fed directly.
            QFile f(QString::fromLocal8Bit(argv[++i]));
            const int columns = QByteArray(argv[++i]).toInt();
            const int rows = QByteArray(argv[++i]).toInt();
            if (!f.open(QIODevice::ReadOnly) || columns < 1 || rows < 1) {
                QTextStream(stderr) << "usage: --file PATH COLUMNS ROWS\n";
                return 2;
            }
            Case c = curatedCase(f.fileName(), columns, rows, f.readAll(), {});
            run(c);
            c.name += QStringLiteral(" (against Ghostty)");
            c.random = true;
            run(c);
            QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
            return failures ? 1 : 0;
        }
        else
            seeds = QByteArray(argv[i]).toInt();
    }
    if (!toReduce.isEmpty()) {
        for (int seed : toReduce)
            reduce(randomCase(quint32(seed)));
        return 0;
    }

    for (const Case &c : curated())
        run(c);
    for (int s = 1; s <= seeds; ++s)
        run(randomCase(quint32(s)));

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
