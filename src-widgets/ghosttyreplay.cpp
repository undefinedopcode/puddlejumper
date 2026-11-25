/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "ghosttyreplay.h"

#include <algorithm>

namespace GhosttySnapshot {

namespace {

constexpr char ESC = '\033';
const QByteArray ST = QByteArrayLiteral("\033\\");

QByteArray num(qint64 n) { return QByteArray::number(n); }

// Control characters out of text that goes inside an OSC.
QByteArray oscText(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    for (char c : in) {
        const auto u = quint8(c);
        if (u >= 0x20 && u != 0x7F)
            out += c;
    }
    return out;
}

void appendUtf8(QByteArray &out, char32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

void appendColor(QByteArray &out, const Color &c, int base, int brightBase)
{
    // Base colours as 30-37/90-97 (and 40-47/100-107) so the target applies
    // its own rules to them (bold as bright, its scheme's intense colours).
    if (c.kind == Color::Palette) {
        if (c.index < 8)
            out += ';' + num(base + c.index);
        else if (c.index < 16)
            out += ';' + num(brightBase + c.index - 8);
        else
            out += ';' + num(base + 8) + ";5;" + num(c.index);
    } else if (c.kind == Color::Rgb) {
        out += ';' + num(base + 8) + ";2;" + num(c.r) + ';' + num(c.g) + ';' + num(c.b);
    }
}

bool sameLink(const Hyperlink &a, const Hyperlink &b)
{
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case Hyperlink::None: return true;
    case Hyperlink::Implicit: return a.implicitId == b.implicitId && a.uri == b.uri;
    case Hyperlink::Explicit: return a.id == b.id && a.uri == b.uri;
    }
    return false;
}

// Whether a cell shows anything: an empty cell with no background,
// decoration or link can be left out at the end of a row.
bool isBlank(const Page &page, const Cell &c)
{
    if (c.content() != Cell::Codepoint || c.codepoint() != 0 || c.hyperlinkId() != 0
        || c.width() != Cell::Narrow)
        return false;
    if (c.styleId() == 0)
        return true;
    const Style s = page.styles.value(c.styleId());
    return s.background.kind == Color::None && !s.has(Style::Inverse)
        && s.underline == Style::NoUnderline && !s.has(Style::Strikethrough)
        && !s.has(Style::Overline);
}

struct RowRef
{
    const Page *page = nullptr;
    int y = 0;
    const Row &row() const { return page->rowList[y]; }
};

// The rows of a screen's pages, oldest first, with what is pinned to them.
struct Rows
{
    QList<RowRef> rows;
    // Index of the first row of each of screen.pages.
    QList<int> pageStart;
};

Rows screenRows(const Screen &s, bool withHistory)
{
    Rows r;
    if (withHistory) {
        for (auto it = s.history.crbegin(); it != s.history.crend(); ++it)
            for (int y = 0; y < it->rowList.size(); ++y)
                r.rows.append({&*it, y});
    }
    for (const Page &p : s.pages) {
        r.pageStart.append(int(r.rows.size()));
        for (int y = 0; y < p.rowList.size(); ++y)
            r.rows.append({&p, y});
    }
    return r;
}

class Writer
{
public:
    Writer(const Snapshot &s, const ReplayOptions &o) : m_snap(s), m_term(s.terminal), m_opt(o) {}

    QByteArray run();

private:
    void csi(const QByteArray &body) { m_out += ESC; m_out += '['; m_out += body; }
    void osc(const QByteArray &body) { m_out += ESC; m_out += ']'; m_out += body; m_out += ST; }
    void cup(int y, int x) { csi(num(y + 1) + ';' + num(x + 1) + 'H'); }

    void setStyle(const Style &s);
    void setLink(const Hyperlink &l);
    void resetPen()
    {
        setLink(Hyperlink());
        setStyle(Style());
    }

    void terminalState();
    void tabStops();
    void screenContent(const Screen &s, bool primary);
    // The cells of one row from the cursor's column 0; returns how many
    // columns were written.
    int row(const RowRef &ref);
    void placementsOn(const Screen &s, const Rows &rows, int index, bool scrolling);
    void place(const KittyPlacement &p);
    void images(const Screen &s);
    void cursorState(const Screen &s, const Rows &rows);
    void charsets(const Charsets &c);
    void modes();

    const Snapshot &m_snap;
    const TerminalState &m_term;
    const ReplayOptions &m_opt;
    QByteArray m_out;
    Style m_style;
    Hyperlink m_link;
};

void Writer::setStyle(const Style &s)
{
    if (s == m_style)
        return;
    m_out += Replay::sgr(s, m_opt.colonSgr);
    m_style = s;
}

void Writer::setLink(const Hyperlink &l)
{
    if (sameLink(l, m_link))
        return;
    if (l.kind == Hyperlink::None)
        osc("8;;");
    else if (l.kind == Hyperlink::Explicit)
        osc("8;id=" + oscText(l.id).replace(':', "").replace(';', "") + ';' + oscText(l.uri));
    else
        osc("8;;" + oscText(l.uri));
    m_link = l;
}

int Writer::row(const RowRef &ref)
{
    const Row &r = ref.row();
    const Page &page = *ref.page;
    const int columns = m_term.columns;

    if (m_opt.promptMarks && r.prompt == Row::PromptStart)
        osc("133;A");

    // A wrapped row is written to its end so the next one follows by
    // wrapping (see below); elsewhere trailing blanks are left out.
    int end = columns;
    if (!r.wrap)
        while (end > 0 && isBlank(page, r.cell(end - 1)))
            --end;

    int written = 0;
    for (int x = 0; x < end; ++x) {
        const Cell c = r.cell(x);
        // A wide character's right half; or the cell left empty at the end
        // of a row because the wide character after it didn't fit, which
        // the target leaves empty itself when it wraps that character.
        if (c.width() == Cell::SpacerTail || c.width() == Cell::SpacerHead)
            continue;

        Style style = c.styleId() ? page.styles.value(c.styleId()) : Style();
        char32_t cp = c.codepoint();
        if (c.content() == Cell::PaletteBackground) {
            style.background = Color{Color::Palette, c.paletteIndex(), 0, 0, 0};
        } else if (c.content() == Cell::RgbBackground) {
            const quint32 rgb = c.rgb();
            style.background = Color{Color::Rgb, 0, quint8(rgb >> 16), quint8(rgb >> 8), quint8(rgb)};
        }
        if (cp == 0)
            cp = U' ';

        if (x > written)
            csi(num(x + 1) + 'G');  // only after a spacer: keep to Ghostty's columns
        setStyle(style);
        setLink(c.hyperlinkId() ? page.hyperlinks.value(c.hyperlinkId()) : Hyperlink());
        appendUtf8(m_out, cp);
        const QList<char32_t> extra = page.grapheme(x, ref.y);
        for (char32_t e : extra)
            appendUtf8(m_out, e);

        const int width = c.width() == Cell::Wide ? 2 : 1;
        written = x + width;
        // The target may measure this character differently (wide or not,
        // a cluster or several): put the cursor where Ghostty had it.
        if ((width == 2 || !extra.isEmpty()) && written < columns)
            csi(num(written + 1) + 'G');
    }
    // A wrapped row ends waiting to wrap, whatever follows: a wide
    // character that didn't fit left its last cell empty.
    if (r.wrap && written < columns) {
        resetPen();  // the cursor is at `written`
        m_out += QByteArray(columns - written, ' ');
        written = columns;
    }
    return written;
}

void Writer::place(const KittyPlacement &p)
{
    QByteArray keys = "a=p,q=2,C=1,i=" + num(p.key.imageId);
    // An id the terminal made up is left for the target to make up again.
    if (p.key.external)
        keys += ",p=" + num(p.key.id);
    if (p.location == KittyPlacement::Virtual)
        keys += ",U=1";
    if (p.location == KittyPlacement::Relative) {
        keys += ",P=" + num(p.parent.imageId);
        if (p.parent.external)
            keys += ",Q=" + num(p.parent.id);
        if (p.horizontalOffset)
            keys += ",H=" + num(p.horizontalOffset);
        if (p.verticalOffset)
            keys += ",V=" + num(p.verticalOffset);
    }
    if (p.xOffset)
        keys += ",X=" + num(p.xOffset);
    if (p.yOffset)
        keys += ",Y=" + num(p.yOffset);
    if (p.sourceX)
        keys += ",x=" + num(p.sourceX);
    if (p.sourceY)
        keys += ",y=" + num(p.sourceY);
    if (p.sourceWidth)
        keys += ",w=" + num(p.sourceWidth);
    if (p.sourceHeight)
        keys += ",h=" + num(p.sourceHeight);
    if (p.columns)
        keys += ",c=" + num(p.columns);
    if (p.rows)
        keys += ",r=" + num(p.rows);
    if (p.z)
        keys += ",z=" + num(p.z);
    m_out += "\033_G" + keys + ST;
}

// Placements pinned to row `index` of `rows`, which the cursor is on.
void Writer::placementsOn(const Screen &s, const Rows &rows, int index, bool scrolling)
{
    if (!m_opt.kittyImages)
        return;
    const RowRef &ref = rows.rows[index];
    QList<const KittyPlacement *> here;
    for (const KittyPlacement &p : ref.page->kittyPlacements)  // history pages
        if (p.location == KittyPlacement::Pin && p.y == ref.y)
            here.append(&p);
    if (s.kitty) {
        for (const KittyPlacement &p : s.kitty->placements) {
            if (p.location != KittyPlacement::Pin || p.page >= rows.pageStart.size())
                continue;
            if (rows.pageStart[p.page] + p.y == index)
                here.append(&p);
        }
    }
    if (here.isEmpty())
        return;
    resetPen();
    m_out += "\0337";  // DECSC: the row's pending wrap comes back with DECRC
    for (const KittyPlacement *p : here) {
        if (scrolling)
            m_out += '\r';
        else
            cup(index - (int(rows.rows.size()) - m_term.rows), 0);
        if (p->x)
            csi(num(p->x) + 'C');
        place(*p);
    }
    m_out += "\0338";
}

void Writer::images(const Screen &s)
{
    if (!m_opt.kittyImages || !s.kitty)
        return;
    resetPen();
    for (const KittyImage &img : s.kitty->images) {
        if (img.data.isEmpty())
            continue;  // its pixels never came
        QByteArray data = img.data;
        int f = 32;
        switch (img.format) {
        case KittyRgb: f = 24; break;
        case KittyRgba: f = 32; break;
        case KittyGray:
        case KittyGrayAlpha: {
            // The protocol has no grey formats; Ghostty keeps them from PNGs.
            const bool alpha = img.format == KittyGrayAlpha;
            const int step = alpha ? 2 : 1;
            QByteArray rgb;
            rgb.reserve(data.size() / step * (alpha ? 4 : 3));
            for (qsizetype i = 0; i + step <= data.size(); i += step) {
                rgb += data[i];
                rgb += data[i];
                rgb += data[i];
                if (alpha)
                    rgb += data[i + 1];
            }
            data = rgb;
            f = alpha ? 32 : 24;
            break;
        }
        case KittyPng: continue;  // never stored
        }
        const QByteArray id = num(img.id);
        m_out += Replay::kittyTransmit("a=t,q=2,f=" + num(f) + ",s=" + num(img.width) + ",v="
                                           + num(img.height) + ",i=" + id,
                                       data);
        if (!img.animated)
            continue;
        for (const KittyFrame &fr : img.frames) {
            if (fr.data.isEmpty())
                break;  // later frames would land at the wrong numbers
            m_out += Replay::kittyTransmit("a=f,q=2,f=32,s=" + num(img.width) + ",v="
                                               + num(img.height) + ",i=" + id + ",z=" + num(fr.gapMs),
                                           fr.data);
        }
        QByteArray control = "a=a,q=2,i=" + id;
        if (img.rootGapMs)
            m_out += "\033_Ga=a,q=2,i=" + id + ",r=1,z=" + num(img.rootGapMs) + ST;
        // Ghostty keeps loops (v - 1); 0 is forever.
        control += ",v=" + num(qint64(img.maxLoops) + 1);
        control += ",c=" + num(qint64(img.currentIndex) + 1);
        control += ",s=" + num(img.animationState == 2 ? 3 : img.animationState == 1 ? 2 : 1);
        m_out += "\033_G" + control + ST;
    }
}

void Writer::screenContent(const Screen &s, bool primary)
{
    const Rows rows = screenRows(s, primary);
    const int first = primary ? 0 : std::max(0, int(rows.rows.size()) - m_term.rows);

    images(s);

    bool previousWrapped = false;
    for (int i = first; i < rows.rows.size(); ++i) {
        // A wrapped row's successor follows it by wrapping, which keeps the
        // target's wrapped-line flag. Otherwise on the primary screen rows
        // scroll up from the bottom into the target's history; on the
        // alternate screen each goes in its place.
        if (i > first && !previousWrapped) {
            resetPen();
            if (primary)
                m_out += "\r\n";
            else
                cup(i - first, 0);
        }
        const int written = row(rows.rows[i]);
        // A wrapped row with nothing after it (erased since): a blank
        // makes the wrap happen, so the target marks the row wrapped too.
        if (previousWrapped && written == 0 && i > first) {
            resetPen();
            m_out += ' ';
        }
        previousWrapped = rows.rows[i].row().wrap;
        placementsOn(s, rows, i, primary);
    }
    resetPen();

    if (m_opt.kittyImages && s.kitty) {
        // Not tied to a cell: in the order stored, parents before children,
        // then those that came with history pages.
        for (const KittyPlacement &p : s.kitty->placements)
            if (p.location != KittyPlacement::Pin)
                place(p);
        if (primary)
            for (auto it = s.history.crbegin(); it != s.history.crend(); ++it)
                for (const KittyPlacement &p : it->kittyPlacements)
                    if (p.location != KittyPlacement::Pin)
                        place(p);
    }
}

void Writer::charsets(const Charsets &c)
{
    static const char designator[4] = {'(', ')', '*', '+'};
    for (int i = 0; i < 4; ++i) {
        const char set = c.g[i] == 3 ? '0' : c.g[i] == 2 ? 'A' : 'B';
        m_out += ESC;
        m_out += designator[i];
        m_out += set;
    }
    // GL: SI, SO, LS2, LS3. GR: LS1R, LS2R, LS3R (G0 can't be GR).
    static const char *gl[4] = {"\x0f", "\x0e", "\033n", "\033o"};
    static const char *gr[4] = {"", "\033~", "\033}", "\033|"};
    m_out += gl[c.gl & 3];
    m_out += gr[c.gr & 3];
}

void Writer::modes()
{
    const Modes &m = m_term.currentModes;
    // Screens, origin, insert and wrap are dealt with where they matter;
    // DECCOLM would clear and resize.
    static const int dec[] = {
        // Mode, DEC private number; ANSI modes are separate below.
        ModeCursorKeys, 1,       ModeSlowScroll, 4,         ModeReverseColors, 5,
        ModeAutorepeat, 8,       ModeMouseX10, 9,           ModeCursorBlinking, 12,
        ModeEnableMode3, 40,     ModeReverseWrap, 45,       ModeKeypadKeys, 66,
        ModeBackarrowKey, 67,    ModeMouseNormal, 1000,     ModeMouseButton, 1002,
        ModeMouseAny, 1003,      ModeFocusEvent, 1004,      ModeMouseUtf8, 1005,
        ModeMouseSgr, 1006,      ModeMouseAlternateScroll, 1007, ModeMouseUrxvt, 1015,
        ModeMouseSgrPixels, 1016, ModeIgnoreKeypadWithNumlock, 1035, ModeAltEscPrefix, 1036,
        ModeAltSendsEscape, 1039, ModeReverseWrapExtended, 1045, ModeBracketedPaste, 2004,
        ModeGraphemeCluster, 2027, ModeReportColorScheme, 2031, ModeReportVisibility, 2033,
        ModeInBandSizeReports, 2048, ModeKittyPasteEvents, 5522, ModeSynchronizedOutput, 2026,
    };
    // Resets first: some terminals treat the mouse modes as one setting,
    // where resetting one would undo another just set.
    QByteArray off, on;
    for (size_t i = 0; i < std::size(dec); i += 2) {
        QByteArray &list = m.has(Mode(dec[i])) ? on : off;
        if (!list.isEmpty())
            list += ';';
        list += num(dec[i + 1]);
    }
    if (!off.isEmpty())
        csi('?' + off + 'l');
    if (!on.isEmpty())
        csi('?' + on + 'h');

    // ANSI modes, only away from their usual settings.
    if (m.has(ModeDisableKeyboard))
        csi("2h");
    if (!m.has(ModeSendReceive))
        csi("12l");
    if (m.has(ModeLinefeed))
        csi("20h");
}

void Writer::tabStops()
{
    const QList<bool> &stops = m_term.tabStops;
    bool standard = true;
    for (int x = 1; x < stops.size(); ++x)
        if (stops[x] != (x % 8 == 0))
            standard = false;
    if (standard)
        return;
    csi("3g");
    for (int x = 1; x < stops.size(); ++x) {
        if (stops[x]) {
            cup(0, x);
            m_out += "\033H";
        }
    }
    cup(0, 0);
}

void Writer::terminalState()
{
    for (int i = 0; i < 256; ++i)
        if (m_term.paletteOverrides[size_t(i)])
            osc("4;" + num(i) + ';' + Replay::colorSpec(*m_term.paletteOverrides[size_t(i)]));
    // xterm's rgb: form only: qtermwidget asserts on an OSC 11 colour that
    // QColor can parse (#rrggbb).
    if (m_term.foreground.override)
        osc("10;" + Replay::colorSpec(*m_term.foreground.override));
    if (m_term.background.override)
        osc("11;" + Replay::colorSpec(*m_term.background.override));
    if (m_term.cursorColor.override)
        osc("12;" + Replay::colorSpec(*m_term.cursorColor.override));
    if (!m_term.title.isEmpty())
        osc("2;" + oscText(m_term.title));
    if (!m_term.pwd.isEmpty())
        osc("7;" + oscText(m_term.pwd));
}

void Writer::cursorState(const Screen &s, const Rows &rows)
{
    const TerminalState &t = m_term;
    const bool origin = t.currentModes.has(ModeOrigin);

    if (s.savedCursor) {
        const SavedCursor &c = *s.savedCursor;
        // DECSC saves origin mode too; setting it sends the cursor home,
        // so before the move (no margins are set yet).
        if (c.origin)
            csi("?6h");
        cup(c.y, c.x);
        setStyle(c.pen);
        charsets(c.charsets);
        m_out += "\0337";
        if (c.origin)
            csi("?6l");
    }

    modes();
    if (t.scrollTop != 0 || t.scrollBottom + 1 != t.rows)
        csi(num(t.scrollTop + 1) + ';' + num(t.scrollBottom + 1) + 'r');
    if (t.currentModes.has(ModeLeftRightMargin)) {
        csi("?69h");
        if (t.scrollLeft != 0 || t.scrollRight + 1 != t.columns)
            csi(num(t.scrollLeft + 1) + ';' + num(t.scrollRight + 1) + 's');
    }
    if (origin)
        csi("?6h");

    const int y = s.cursorY, x = s.cursorX;
    auto move = [&](int col) {
        if (origin)
            cup(y - t.scrollTop, col - (t.currentModes.has(ModeLeftRightMargin) ? t.scrollLeft : 0));
        else
            cup(y, col);
    };

    // The cursor past the last column, waiting to wrap: write that column
    // again, which leaves it so.
    const int active = int(rows.rows.size()) - t.rows;
    if (s.pendingWrap && active + y >= 0 && active + y < rows.rows.size()) {
        const RowRef &ref = rows.rows[active + y];
        int col = x;
        Cell c = ref.row().cell(col);
        if (c.width() == Cell::SpacerTail && col > 0)
            c = ref.row().cell(--col);
        move(col);
        Style style = c.styleId() ? ref.page->styles.value(c.styleId()) : Style();
        setStyle(style);
        setLink(c.hyperlinkId() ? ref.page->hyperlinks.value(c.hyperlinkId()) : Hyperlink());
        appendUtf8(m_out, c.content() == Cell::Codepoint && c.codepoint() ? c.codepoint() : U' ');
        for (char32_t e : ref.page->grapheme(col, ref.y))
            appendUtf8(m_out, e);
    } else {
        move(x);
    }

    if (t.currentModes.has(ModeInsert))
        csi("4h");
    if (!t.currentModes.has(ModeWraparound))
        csi("?7l");

    setLink(s.cursorHyperlink);
    setStyle(s.pen);
    charsets(s.charsets);
    if (s.charsets.singleShift == 3)
        m_out += "\033N";  // SS2
    else if (s.charsets.singleShift == 4)
        m_out += "\033O";  // SS3

    // DECSCUSR: the shape, blinking or not.
    int shape = 2;  // block
    switch (s.cursorStyle) {
    case CursorBlock:
    case CursorBlockHollow: shape = 2; break;
    case CursorUnderline: shape = 4; break;
    case CursorBar: shape = 6; break;
    }
    if (t.currentModes.has(ModeCursorBlinking))
        --shape;
    csi(num(shape) + " q");

    // The Kitty keyboard flag stack, oldest first: the bottom set, the
    // rest pushed. The stack is a ring of 8 ending at the current entry.
    bool anyFlags = false;
    for (quint8 f : s.kittyKeyboardFlags)
        anyFlags = anyFlags || f;
    if (anyFlags) {
        const int top = s.kittyKeyboardIndex & 7;
        csi('=' + num(s.kittyKeyboardFlags[size_t((top + 1) & 7)]) + ";1u");
        for (int k = 2; k <= 8; ++k)
            csi('>' + num(s.kittyKeyboardFlags[size_t((top + k) & 7)]) + 'u');
    }
    if (t.modifyOtherKeys2)
        csi(">4;2m");

    csi(t.currentModes.has(ModeCursorVisible) ? "?25h" : "?25l");
}

QByteArray Writer::run()
{
    const TerminalState &t = m_term;
    const Screen *primary = m_snap.screen(0);
    const Screen *alternate = m_snap.screen(1);
    const bool onAlternate = t.activeScreen == 1 && alternate;

    // From whatever the target was showing: primary screen, everything
    // cleared, plain pen, the cursor hidden while this draws.
    csi("?25l");
    csi("?1049l");
    m_out += "\033[0m\033(B\x0f";
    csi("r");
    csi("?6l");
    csi("?7h");
    csi("4l");
    csi("H");
    csi("2J");
    csi("3J");

    terminalState();
    tabStops();

    if (primary) {
        screenContent(*primary, true);
        const Rows rows = screenRows(*primary, true);
        if (!onAlternate) {
            cursorState(*primary, rows);
        } else {
            // 1049 keeps the primary cursor in its saved cursor; 1047
            // switches without saving and 47 is the old form of 1047.
            const SavedCursor *saved = primary->savedCursor ? &*primary->savedCursor : nullptr;
            if (t.currentModes.has(ModeAltScreenSaveCursorClearEnter) && saved) {
                cup(saved->y, saved->x);
                setStyle(saved->pen);
                charsets(saved->charsets);
            } else {
                if (saved) {
                    cup(saved->y, saved->x);
                    setStyle(saved->pen);
                    charsets(saved->charsets);
                    m_out += "\0337";
                }
                cup(primary->cursorY, primary->cursorX);
                setStyle(primary->pen);
                charsets(primary->charsets);
            }
        }
    }

    if (onAlternate) {
        if (t.currentModes.has(ModeAltScreenSaveCursorClearEnter))
            csi("?1049h");
        else if (t.currentModes.has(ModeAltScreen))
            csi("?1047h");
        else
            csi("?47h");
        resetPen();
        m_out += "\033(B\x0f";
        csi("H");
        csi("2J");
        screenContent(*alternate, false);
        cursorState(*alternate, screenRows(*alternate, false));
    }

    // Last, so it goes on from where the program left off: an escape
    // sequence the snapshot cut in half.
    m_out += m_snap.continuation;
    return m_out;
}

} // namespace

namespace Replay {

QByteArray sgr(const Style &s, bool colonSgr)
{
    QByteArray out = "\033[0";
    if (s.has(Style::Bold))
        out += ";1";
    if (s.has(Style::Faint))
        out += ";2";
    if (s.has(Style::Italic))
        out += ";3";
    if (s.underline != Style::NoUnderline) {
        if (colonSgr && s.underline != Style::Single)
            out += ";4:" + num(int(s.underline));
        else
            out += ";4";
    }
    if (s.has(Style::Blink))
        out += ";5";
    if (s.has(Style::Inverse))
        out += ";7";
    if (s.has(Style::Invisible))
        out += ";8";
    if (s.has(Style::Strikethrough))
        out += ";9";
    if (s.has(Style::Overline))
        out += ";53";
    appendColor(out, s.foreground, 30, 90);
    appendColor(out, s.background, 40, 100);
    if (colonSgr) {
        const Color &u = s.underlineColor;
        if (u.kind == Color::Palette)
            out += ";58:5:" + num(u.index);
        else if (u.kind == Color::Rgb)
            out += ";58:2::" + num(u.r) + ':' + num(u.g) + ':' + num(u.b);
    }
    out += 'm';
    return out;
}

QByteArray colorSpec(const Rgb &c)
{
    auto hex = [](quint8 v) { return QByteArray::number(v, 16).rightJustified(2, '0'); };
    return "rgb:" + hex(c.r) + '/' + hex(c.g) + '/' + hex(c.b);
}

QByteArray kittyTransmit(const QByteArray &keys, const QByteArray &data)
{
    // The protocol's limit: 4096 bytes of base64 per chunk.
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    qsizetype pos = 0;
    bool first = true;
    do {
        const qsizetype n = std::min<qsizetype>(4096, b64.size() - pos);
        const bool more = pos + n < b64.size();
        out += "\033_G";
        if (first)
            out += keys + ',';
        else
            out += "q=2,";
        out += more ? "m=1;" : "m=0;";
        out += b64.mid(pos, n);
        out += ST;
        pos += n;
        first = false;
    } while (pos < b64.size());
    return out;
}

} // namespace Replay

QByteArray replay(const Snapshot &snapshot, const ReplayOptions &options)
{
    return Writer(snapshot, options).run();
}

} // namespace GhosttySnapshot
