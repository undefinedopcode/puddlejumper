/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "ghosttysnapshot.h"

/*
 * A Ghostty snapshot (see ghosttysnapshot.h) as the bytes that rebuild it in
 * a terminal emulator: scrollback, both screens, cursor, modes, the colours
 * and title a program set, Kitty images and their placements, and last the
 * snapshot's half-parsed escape sequence. TmuxSession restores panes the same
 * way, from capture-pane output.
 *
 * The target is a terminal of the snapshot's size (terminal.columns x
 * terminal.rows) that has been resized first; anything already on it is
 * cleared. Wrapped rows are written without line breaks, so a terminal that
 * rewraps on resize keeps doing so.
 *
 * Not carried: the colours the server was configured with (the client's
 * theme stands), protected cells (DECSCA), modes saved with XTSAVE, the REP
 * character, a Kitty image transmission cut off between chunks, and image
 * numbers (I=): images keep their ids.
 */
namespace GhosttySnapshot {

struct ReplayOptions
{
    // Underline styles (4:2 .. 4:5) and underline colours (58), which take
    // colon subparameters. qtermwidget reads ':' as ';' (4:3 would be
    // underline and italic), so off by default: every underline is single.
    bool colonSgr = false;
    // OSC 133;A at the start of each prompt row. Off by default: in
    // PuddleJumper a prompt starting means a program has just ended.
    bool promptMarks = false;
    // Kitty images re-sent with the graphics protocol, and placed.
    bool kittyImages = true;
};

QByteArray replay(const Snapshot &snapshot, const ReplayOptions &options = {});

// The pieces, for tests.
namespace Replay {
// An SGR sequence that sets exactly `style` (it starts by resetting).
QByteArray sgr(const Style &style, bool colonSgr);
// An xterm colour specification: rgb:rr/gg/bb.
QByteArray colorSpec(const Rgb &c);
// Base64 Kitty graphics commands carrying `data`, split into chunks.
QByteArray kittyTransmit(const QByteArray &keys, const QByteArray &data);
} // namespace Replay

} // namespace GhosttySnapshot
