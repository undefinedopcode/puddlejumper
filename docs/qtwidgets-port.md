# Exploratory: porting SuperTerm to Qt Widgets

Branch: `explore/qtwidgets`. No port code yet — this records what was measured
first, so the decision is made on evidence rather than assumption.

## Why consider it

Three limitations in the QML build trace to a single cause: **QMLTermWidget
exposes far less of the terminal than the Widgets `QTermWidget` does.**

| Setting | QML build | Widgets build |
| --- | --- | --- |
| Scrollback (`setHistorySize`) | **unreachable** — exported symbol but no `Q_PROPERTY`, slot or `Q_INVOKABLE` | `setHistorySize(int)` / `historySize()` public |
| Own line-drawing chars | **unreachable** — inline method, no symbol, no property | `setDrawLineChars(bool)` public |
| Cursor shape (block/underline/beam) | **not offered** | `setKeyboardCursorShape(...)` public |
| Key binding profiles | not exposed | `setKeyBindings(name)` public |
| History saving | not exposed | `saveHistory(QIODevice*)` public |
| Search bar | hand-written overlay | built in (`toggleShowSearchBar()`) |

These are exactly the gaps that made the settings dialog show two controls
read-only and skip cursor shape entirely.

## Verified on this machine (Qt 6.12, qtermwidget6 2.4.0)

A standalone `QApplication` + `QTermWidget` builds and runs with
`pkg-config qtermwidget6`:

- `setHistorySize(8000)` applied, `historySize()` returns `8000`.
- `setDrawLineChars(true)` and `setKeyboardCursorShape(BlockCursor)` applied
  without error (enum lives in `Konsole::Emulation::KeyboardCursorShape`,
  reachable via `#include <Emulation.h>`).
- Shell starts, prompt renders, colour scheme applies.
- **Thumbnails still work**: `Konsole::TerminalDisplay` is discoverable via
  `findChildren<QWidget*>()`, and `QWidget::grab()` on it produced correct
  Solarized content (`#002b36` background confirmed by pixel sampling) across
  four consecutive grabs with the widget still alive afterwards.

That last point matters most: the QML build needed `useFBORendering: false`
because `grabToImage()` corrupted FBO rendering, which is what broke `nano`.
Widgets render to a backing store, so repeated `grab()` is straightforwardly
safe — no equivalent workaround needed.

## Costs, honestly

- **The UI layer is a rewrite.** ~2100 lines of QML. The widgets equivalents
  are `QTabWidget`/`QSplitter`/`QStackedWidget`, `QMenuBar`, `QDialog`,
  `QSlider`, `QComboBox`… none of the layout or styling carries over directly.
  Rough breakdown of what would need re-expressing: 24 layout containers, 13
  labels, 12 shortcuts, 10 tool buttons, 8 sliders, 7 repeaters, plus the
  settings dialog and thumbnail sidebar.
- **Theming changes shape.** `Theme.qml` currently rides on `SystemPalette`
  under the KDE platform theme. In widgets, colours come from `QPalette`
  directly, which is arguably simpler — but the hand-tuned dark fallback would
  need porting, and QSS is a different (and weaker) styling language than QML
  for the custom chrome.
- **"Cross-platform UI consistency" is not automatic.** Widgets inherit the
  *platform* style, so macOS looks like macOS and Windows like Windows unless
  you force Fusion or ship a stylesheet. That is usually a feature, but it is
  the opposite of the pixel-identical look QML gives by default. Worth
  deciding which you actually want.
- **Thumbnails**: viable (proven above), but `findChildren` + class-name
  matching against `Konsole::TerminalDisplay` is the same fragile string
  coupling we already have in the QML build.
- **Licensing is unchanged** — both libraries are the same GPL-2.0-or-later
  Konsole code.

## Things that do NOT improve

- **Windows support.** `qtermwidget6` is the same POSIX-pty-only code as
  QMLTermWidget. A Qt Widgets port buys no cross-platform reach; Windows still
  needs a ConPTY backend either way.
- **The terminal engine itself.** Emulation, Unicode width handling and glyph
  metrics are shared. If the motivation is "nano/ps misbehave" or "glyphs only
  scale at some sizes", a port does not address it — those were app-level bugs
  (now fixed) and font hinting respectively.

## Recommendation

Worth doing **if** the read-only settings are the thing that bothers you. The
widgets API closes every gap listed above, and thumbnails are proven to work
without the FBO workaround.

Not worth doing for cross-platform reach (no gain) or rendering quality (no
change). And it is a real rewrite, not a refactor — a like-for-like UI rebuild
plus a theming rethink.

## Suggested shape if it proceeds

Keep `explore/qtwidgets` as a spike, not a migration: build the shell
(`QMainWindow` + tabs + one `QTermWidget`) and prove the four previously
unreachable settings from the real UI before committing to porting the rest.
That is a few hundred lines and answers the only question that matters — does
the widgets UI feel acceptable to you.

## Spike: built and verified

`spike-widgets/` — a separate `superterm-widgets` target (never touches the QML
app):

```sh
cmake --build build --target superterm-widgets
./build/superterm-widgets              # platform style (here: darkly)
./build/superterm-widgets --style Fusion   # neutral look, same everywhere
```

`QMainWindow` + `QTabWidget` + `QTermWidget`, with a settings dock exposing the
four settings the QML build could not reach. Verified at runtime by driving the
controls and reading the terminal's own state back:

```
SPIKE historySize      = 12345     (spinbox -> setHistorySize)
SPIKE keyBindings      = "default"
SPIKE screen cols/rows = 45 18
SPIKE hasSearchBar     = true       (built in, no overlay to write)
```

Plus `setDrawLineChars()` and `setKeyboardCursorShape()` driven from real
combos, and OSC titles arriving via `QTermWidget::titleChanged`.

### Platform styling

The point you raised: widget styling comes from the platform automatically.
On this machine the spike reports `platform widget style: "darkly"` — your KDE
theme applied with no code from us. `--style Fusion` gives an identical look on
any platform instead, so the choice is available per-build if you want
consistency over native integration.

### Practical notes for a full port

- `QTermWidget::KeyboardCursorShape` aliases the type but not its values; the
  enumerators are in `Konsole::Emulation::KeyboardCursorShape` (`#include
  <Emulation.h>`).
- The terminal display is reachable as a child widget (`findChildren<QWidget*>`
  matching `Konsole::TerminalDisplay`), which is how thumbnails would work —
  with `QWidget::grab()`, and without the FBO workaround.
- `colorScheme()` and `blinkingCursor()` have setters but no getters; track
  those in the app rather than reading them back.
- The QML target must still not link `qtermwidget6`. The spike target does,
  because it does not load the QML plugin — the two can coexist in one
  CMakeLists as separate executables.

## Remaining work if it proceeds

1. Port the chrome: custom tab bar or `QTabWidget` styling, status bar, and the
   settings dialog (QDialog rather than a dock).
2. Decide the thumbnail sidebar: a `QListWidget` of grabbed pixmaps, refreshed
   on a timer, replacing the QML `ListView` version.
3. Port the dark fallback palette to `QPalette`, or adopt the platform palette
   outright.
4. Persistence: `SettingsStore` is already `QSettings`, so it survives largely
   as-is; only the QML-facing wrappers would move.
