# Vendored QTermWidget

Source: [lxqt/qtermwidget](https://github.com/lxqt/qtermwidget) tag `2.4.0`
(commit `4ca192e5d7dc12fe2a06371d3a374d8fa86165af`).

Licence: as upstream. See `LICENSE` (GPL-2.0-or-later) and the per-file headers
(some files are LGPL-2.0-or-later or BSD-3-Clause). Linking it into puddlejumper
means puddlejumper is distributed under GPL-compatible terms.

## What was taken

`lib/` (minus `lib/translations/`), the licence files, `AUTHORS` and `README.md`.
Upstream's `CMakeLists.txt` was replaced; it needs lxqt2-build-tools and installs
a shared library. Ours builds a static library and compiles the keyboard layouts
and colour schemes in as Qt resources under `:/qtermwidget/`. Translations
(search bar strings only, and puddlejumper doesn't use the search bar) are not built.

## Patches

Patches 1–6 are in `lib/TerminalDisplay.cpp` / `.h`; patch 7 also touches
`qtermwidget`, `Session`, `Emulation` and `Vt102Emulation`; patch 10 is in
`lib/Session.cpp`; patch 11 is in `lib/TerminalDisplay.cpp` and
`lib/qtermwidget.cpp`. Each is marked
with a `puddlejumper:` comment.

1. **Block elements and Powerline separators are drawn as geometry.**
   U+2580–U+259F (halves, eighths, shades, quadrants) are filled to exact cell
   boundaries with antialiasing off, so adjacent cells meet with no seam,
   including under fractional scaling. U+E0B0–U+E0BF (triangles, half circles,
   diagonals) are drawn as antialiased paths. Upstream only draws box drawing
   (U+2500–U+257F) itself; for everything else it uses the font's glyph, which is
   sized to the font's ascent/descent rather than the integer cell, so rows and
   columns of block art showed hairline gaps at many point sizes. These two
   ranges are drawn regardless of `setDrawLineChars()`, which still governs box
   drawing.

2. **Backgrounds and text are painted in separate passes.** `paintEvent()` paints
   all fragment backgrounds first, then all text. Upstream painted fragment by
   fragment, so each background covered any part of the previous glyph that
   overhung into it. The text pass covers one extra cell either side of each
   repainted rect so overhang into the repainted area is restored.

3. **Over-wide glyphs are no longer clipped.** `QPainter::drawText(QRect, …)`
   clips to the rect unless `Qt::TextDontClip` is passed. Upstream drew
   "too wide" glyphs (advance ≥ 2 cells, typically a fallback font's symbol) into
   a one-cell rect, so they were cut in half. They are now unclipped and centred
   on their cell.

4. **Glyph widths above U+FFFF are measured correctly.** `QChar(c)` truncates to
   16 bits, so Nerd Font icons at U+F0000+ were measured as some unrelated BMP
   character. `charAdvance()` uses `QString::fromUcs4` for those.

5. **Drawn characters take exactly one cell in `textWidth()`**, rather than the
   advance of whatever glyph the font (or a fallback) has for them, which could
   misplace a fragment of box drawing in fonts lacking those glyphs.

6. **`cellArea()` accessor.** Returns where the character grid sits in the
   widget, which puddlejumper needs to snap screenshot selections to cells.
   Upstream keeps the margins and scrollbar offset private.

7. **Driving a terminal from outside**, for tmux control mode.
   `QTermWidget::feedData()` hands output straight to the emulator (the
   teletype mode's pty would otherwise translate `\n` to `\r\n`);
   `setFixedTerminalSize()` pins the screen to the pane size tmux decides,
   rather than whatever fits the widget; and `setAnswerTerminalQueries(false)`
   stops the emulator replying to device-attribute and cursor-position queries
   that tmux has already answered, which would otherwise arrive as typed input.
   With the size fixed, a view that resizes (a hidden tab being shown) is told
   to redraw by `Session::updateTerminalSize()`; normally the screen resizing
   is what prompts that, and without it the view kept a stale image.

8. **Rendering the screen without the widget**, for the sidebar's previews.
   `QTermWidget::renderScreen()` paints the current screen (primary or
   alternate, its visible page) from the emulator's character image into a
   `QImage` at any scale, with the display's font and colours, and
   `screenPixelSize()` gives its natural size. A hidden tab's display never
   paints, so `grab()` returned nothing for it; this works whatever the
   widget's state. It is a simplified painter: colours, bold, italic,
   underline, faint, wide characters and the cursor, but no blinking,
   selection or specially drawn line characters.

9. **String sequences are skipped, not printed.** Upstream's tokenizer parses
   OSC (`ESC ]`) but not the other sequences that carry a string up to the
   String Terminator: DCS (`ESC P`), SOS (`ESC X`), PM (`ESC ^`), APC (`ESC _`)
   and screen's window title (`ESC k`). It printed their contents as text.
   Under tmux, oh-my-zsh and others set the window name with `ESC k`<name>`ESC
   \` before every command, and tmux control mode passes pane output on as it
   is, so the command's name appeared in the output. All five are now consumed
   up to `ESC \` (or BEL, as OSC allows) and ignored.

10. **The shell's current directory is found on macOS.**
    `Session::currentWorkingDirectory()` reads `/proc/<pid>/cwd` on Linux only;
    elsewhere it returned the directory the shell started in. On macOS it now
    asks `proc_pidinfo(PROC_PIDVNODEPATHINFO)`, so new tabs and saved
    workspaces follow the shell after a `cd` there too.

11. **Text stays on the cell grid when the font's advance is fractional.**
    The cell width is whole pixels, but a run of characters is drawn as one
    string at the font's own advance. Where hinting snaps that to whole pixels
    (Linux with `PreferFullHinting`) the two agree; Core Text never hints, so
    on macOS text drifted from the grid and the cursor by up to 33px over 80
    columns at all but a few sizes (Monaco was right only at 10 and 15px).
    `setVTFont()` now gives a monospaced font letter spacing of the cell width
    minus its advance, so every glyph advances exactly one cell.
    `renderScreen()` (patch 8) measures the font without that spacing before
    applying its own.

12. **Images, by kitty's graphics protocol.** New `lib/TerminalGraphics.*`
    keeps each screen's images and placements (the normal and alternate
    screens have their own, as in kitty) and carries out the commands
    (`ESC _ G … ESC \`): transmitting (in the escape code, in chunks, or from
    a file, temporary file or POSIX shared memory; as RGB, RGBA, PNG or
    indexed colour, zlib-compressed or not), placing (source rectangle, cell
    offset, columns and rows, z-index, `C=1`), querying (with `e=` feature
    queries), deleting, and animation (frames, composing, playing), with
    kitty's 320 MB quota a screen. Files are checked before opening (regular,
    not under `/proc`, `/sys` or `/dev`), and every failure to read one gets
    the same answer; a temporary file is deleted only from a temporary
    directory and with `tty-graphics-protocol` in its name. Indexed images
    keep their palette indices and take the display's colours when drawn.
    Animations advance when drawn (`animate()`), the display coming back when
    the next frame is due, and only while in view. The tokenizer
    in `Vt102Emulation` collects the command (up to 64 MB, as it now does a
    tmux passthrough too). `Screen` tells the graphics when lines move,
    scroll into the history, or are cleared, so placements stay with their
    text. `TerminalDisplay` draws them in kitty's layers: under cell
    backgrounds, under the text, over it. Unicode placeholders (`U=1`
    virtual placements, shown by cells holding U+10EEEE, numbered by the
    combining marks in `lib/PlaceholderDiacritics.h`) work too; that is how
    images get through tmux. The pty's size now carries pixels
    (`ws_xpixel`, `ws_ypixel`), and `CSI 14 t` and `CSI 16 t` report the
    window's and a cell's size in pixels. Not done: relative placements, and
    placement ids in the underline colour.

13. **A CSI sequence with an intermediate byte it doesn't know is ignored.**
    Such a sequence (`CSI * x`, DECSACE, say) used to end the escape at the
    intermediate, so its final byte was printed: kitty's `icat` left an "x"
    before its images.

14. **Combining characters past U+FFFF combine.** `displayCharacter()` asked
    `QChar(c).category()`, which cut such a character to 16 bits; it now asks
    `QChar::category(c)`. And U+10EEEE, a private-use character, always takes
    one cell.

The build also defines `HAVE_UTMPX` on macOS (in our `CMakeLists.txt`, not the
sources): it has only the utmpx API, and `kpty.cpp` otherwise falls back to
utmp functions that don't exist there.

## Updating

Diff this tree against the upstream tag it came from, re-apply the
`puddlejumper:`-marked changes onto the new tag, and update the version and commit
above and in `CMakeLists.txt`. If upstream adds or removes `lib/*.cpp` files,
update the source list in `CMakeLists.txt`.
