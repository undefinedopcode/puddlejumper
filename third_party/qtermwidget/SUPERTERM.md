# Vendored QTermWidget

Source: [lxqt/qtermwidget](https://github.com/lxqt/qtermwidget) tag `2.4.0`
(commit `4ca192e5d7dc12fe2a06371d3a374d8fa86165af`).

Licence: as upstream. See `LICENSE` (GPL-2.0-or-later) and the per-file headers
(some files are LGPL-2.0-or-later or BSD-3-Clause). Linking it into superterm
means superterm is distributed under GPL-compatible terms.

## What was taken

`lib/` (minus `lib/translations/`), the licence files, `AUTHORS` and `README.md`.
Upstream's `CMakeLists.txt` was replaced; it needs lxqt2-build-tools and installs
a shared library. Ours builds a static library and compiles the keyboard layouts
and colour schemes in as Qt resources under `:/qtermwidget/`. Translations
(search bar strings only, and superterm doesn't use the search bar) are not built.

## Patches

Patches 1–6 are in `lib/TerminalDisplay.cpp` / `.h`; patch 7 also touches
`qtermwidget`, `Session`, `Emulation` and `Vt102Emulation`. Each is marked
with a `superterm:` comment.

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
   widget, which superterm needs to snap screenshot selections to cells.
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

## Updating

Diff this tree against the upstream tag it came from, re-apply the
`superterm:`-marked changes onto the new tag, and update the version and commit
above and in `CMakeLists.txt`. If upstream adds or removes `lib/*.cpp` files,
update the source list in `CMakeLists.txt`.
