/* $XFree86: xc/programs/xterm/wcwidth.character,v 1.3 2001/07/29 22:08:16 tsi Exp $ */
/*
 * This is an implementation of wcwidth() and wcswidth() as defined in
 * "The Single UNIX Specification, Version 2, The Open Group, 1997"
 * <http://www.UNIX-systems.org/online.html>
 *
 * Markus Kuhn -- 2001-01-12 -- public domain
 */

#include <QString>

#ifdef HAVE_UTF8PROC
#include <utf8proc.h>
#else
#include <cwchar>
#endif
#include <algorithm>
#include <iterator>

#include "konsole_wcwidth.h"

#ifdef Q_OS_WIN
// puddlejumper: the C library there has no wcwidth(). Widths as a terminal's
// programs expect them: 0 for combining and format characters, 2 for East
// Asian wide and fullwidth characters and emoji, 1 otherwise.
namespace {

struct Range { char32_t first, last; };

// East Asian Width W and F (Unicode 16), merged where nothing lies between.
constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
    {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2630, 0x2637}, {0x2648, 0x2653},
    {0x267F, 0x267F}, {0x268A, 0x268F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB},
    {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705},
    {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E}, {0x3041, 0x4DBF}, {0x4E00, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4}, {0x16FF0, 0x16FF1}, {0x17000, 0x18CFF},
    {0x18D00, 0x18D08}, {0x1AFF0, 0x1B2FF}, {0x1D300, 0x1D356}, {0x1D360, 0x1D376}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
    {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89},
    {0x1FA8F, 0x1FAC6}, {0x1FACE, 0x1FADC}, {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8}, {0x20000, 0x2FFFD},
    {0x30000, 0x3FFFD},
};

int charWidth(char32_t ucs)
{
    if (ucs == 0)
        return 0;
    if (ucs < 0x20 || (ucs >= 0x7F && ucs < 0xA0))
        return -1;
    if (ucs < 0x300)
        return 1;   // (the soft hyphen, a format character, has a width)
    switch (QChar::category(ucs)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
        return 0;
    default:
        break;
    }
    // Hangul medial vowels and final consonants join the syllable before.
    if ((ucs >= 0x1160 && ucs <= 0x11FF) || (ucs >= 0xD7B0 && ucs <= 0xD7FF))
        return 0;
    // (Sorted: the first range ending at or after it is the only candidate.)
    const Range *range = std::lower_bound(std::begin(kWide), std::end(kWide), ucs,
                                          [](const Range &r, char32_t c) { return r.last < c; });
    return range != std::end(kWide) && ucs >= range->first ? 2 : 1;
}

}
#endif

int konsole_wcwidth(char32_t ucs)
{
#ifdef Q_OS_WIN
    return charWidth(ucs);
#elif defined(HAVE_UTF8PROC)
    utf8proc_category_t cat = utf8proc_category( ucs );
    if (cat == UTF8PROC_CATEGORY_CO) {
        // Co: Private use area. libutf8proc makes them zero width, while tmux
        // assumes them to be width 1, and glibc's default width is also 1
        return 1;
    }
    return utf8proc_charwidth( ucs );
#else
    return wcwidth( wchar_t(ucs) );
#endif
}

// single byte char: +1, multi byte char: +2
int string_width( const std::u32string & wstr )
{
    int w = 0;
    for ( size_t i = 0; i < wstr.length(); ++i ) {
        w += konsole_wcwidth( wstr[ i ] );
    }
    return w;
}
