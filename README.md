# SuperTerm

A modern terminal built on **Qt Widgets** and
**[QTermWidget](https://github.com/lxqt/qtermwidget)**.

Tabs and split panes, native tmux integration (tmux windows as tabs, panes as
panes), live colour-scheme switching, a monospace font picker, font zoom,
working scrollback, cursor shape, a thumbnail sidebar with live previews, and a
keyboard-driven UI — with the terminal emulation itself (VT102/xterm, Unicode,
256-colour, key bindings) handled by QTermWidget's Konsole-derived engine.

The UI chrome follows the platform's widget style, so it looks native on
Plasma, GNOME, macOS and Windows alike; `--style Fusion` forces a consistent
look instead.

## Screenshots

The window chrome, toolbar, tab strip, status bar, and a live zsh prompt:

```
┌──────────────────────────────────────────────────────────┐
│  ⊞▾  ⊠ │ ⧉  ▤  ⌕ │ ⊖ 13 pt ⊕ │ ◧                      ⚙  │
├──────────────────────────────────────────────────────────┤
│  Terminal  ✕                                             │
├──────────────────────────────────────────────────────────┤
│  ~/projects/superterm  on  main ?5        at 02:19:38 PM │
│  ❯                                                       │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ /home/april/projects/superterm        Solarized    13pt  │
└──────────────────────────────────────────────────────────┘
```

## Requirements

| Component | Version |
| --- | --- |
| Qt | 6.5+ (developed against 6.12) |
| CMake | 3.21+ |
| Compiler | C++17 |

QTermWidget is **vendored** in `third_party/qtermwidget` (2.4.0, with rendering
patches) and built as a static library, so no system qtermwidget package is
needed. Its keyboard layouts and colour schemes are compiled in as Qt
resources. See `third_party/qtermwidget/SUPERTERM.md` for what differs from
upstream.

The legacy Qt Quick build needs the separate **QMLTermWidget** module instead;
see the project layout section.

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/superterm
```

Environment overrides, handy for testing:

```sh
SUPERTERM_SHELL=/bin/fish SUPERTERM_SHELL_ARGS="-l" ./build/superterm
SUPERTERM_TMUX="tmux -L test" ./build/superterm   # a separate tmux server
```

### Install

```sh
cmake --install build --prefix ~/.local
```

This installs the binary, a `superterm.desktop` entry and the icon, so the app
appears in your launcher. (Installing to `~/.local` also gives xdg-desktop-
portal an app id to match, which silences the startup portal warning you get
when running straight from the build tree.)

## Settings

Settings live in a centered modal (`Ctrl+,` or the toolbar's settings button),
with four sections:

| Section | Options |
| --- | --- |
| Appearance | **Colours:** scheme (and its editor), background opacity · **Font:** family, size (8–32pt in 0.5pt steps), line spacing, margin · **Rendering:** bright colours in bold, italic, antialiasing, own box drawing, bidirectional text |
| Terminal | **Cursor:** shape, blinking · **Scrollback:** lines kept, scrollbar position · **Input:** confirm multi-line paste, trim trailing newlines, Ctrl+S notice |
| Tabs & Window | **Tabs:** layout · **Watch a Terminal:** activity, silence (with idle time) · **Quitting:** warn while programs are running |
| Profiles | create, duplicate, rename, delete and edit terminal profiles, and choose which one New Tab opens |

Every page uses the same structure, titled groups whose label column is the
same width throughout, so fields don't shift when switching sections. Only
fixed-pitch font families are offered, since the terminal cell grid depends on
them. Changes apply to open tabs immediately, scrollback depth included.

`appearance/lineSpacing` is shared with the QML build. `QTermWidget` itself has
no setter for it, but its display does, and the vendored copy makes that
reachable, so the key now works here too.

Persistence uses `QSettings`:

| Platform | Location |
| --- | --- |
| Linux | `~/.config/superterm/superterm.conf` |
| macOS | `~/Library/Preferences/com.superterm.superterm.plist` |

Stored keys:

```ini
[appearance]
colorScheme=Nord
fontFamily=Hack
fontSize=18
opacity=0.9
blinkingCursor=true
boldIntense=true
italic=true
antialias=true
drawLineChars=true
lineSpacing=0
tabLayout=standard
cursorShape=0
terminalMargin=4
bidi=true
opacity=1

[scrolling]
scrollback=5000
scrollbarPosition=right
flowControlWarning=true
confirmCloseWithRunningProcesses=true
monitorActivity=false
monitorSilence=false
silenceTimeout=10

[input]
confirmMultilinePaste=false
trimPastedTrailingNewlines=false

[window]
x=100
y=80
width=1100
height=700
maximized=false
```

Appearance changes are written as they happen; window geometry is saved when
the window closes. Delete the file to return to defaults.

### Tab names

Tab labels follow the terminal session title, which comes from the standard
**OSC 0 / OSC 2** escape sequences (`ESC ] 0 ; title BEL`). Set one and the tab
and window title update immediately:

```sh
printf '\033]2;my build\007'
```

Shells commonly do this already, so with a configured prompt your tabs often
name themselves. Titles are not persisted; a tab reverts to `Terminal` on
restart until its shell sets one again. An empty title is ignored rather than
blanking the label.

Titles arrive via `QTermWidget::titleChanged`.

### Quitting with programs running

Closing the window (or the Quit action, `Ctrl+Shift+Q`) checks whether any
terminal has a **foreground program** and, if so, asks first:

> A program is still running in one terminal.
> Quitting will terminate micro (micro notes.md). Any unsaved work will be lost.
>
> [Quit Anyway] [Cancel]

**Cancel is the default button and the Escape action**, so a stray Enter or Esc
does nothing destructive.

Detection uses the pty's foreground process group (`tcgetpgrp`, surfaced as
`QTermWidget::getForegroundProcessId()`). It is compared against the *shell's own
process group*, not its PID: equal means the shell is reading the terminal, i.e.
an idle prompt (or one with only background jobs), and no warning is shown.
Different means a foreground program owns the tty and would be killed. The
program name comes from `/proc/<pid>/comm`.

Consequences worth knowing:

- A window full of idle shells never prompts. Only a genuinely running
  foreground program (an editor, a build, `ssh`) triggers it.
- Background jobs (`cmd &`) do not trigger it — they are not in the foreground
  process group, and quitting a terminal does not kill them.
- A program that has exited but whose shell has not yet reaped it can, very
  briefly, look busy.

Turn it off with **Warn before quitting with programs running**
(`scrolling/confirmCloseWithRunningProcesses`). The check is skipped entirely when
the setting is off, so quitting is instantaneous.

(The paragraph below describes the legacy Qt Quick build, where the tab model
was also the pane model. It is kept because the reasoning is easy to repeat:
see `docs/qtwidgets-port.md`.)

Titles are held in a `ListModel`, not a plain array, and that is load-bearing:
the terminal panes live in a `Repeater` keyed on the tab model, so replacing
the model destroys and rebuilds every pane — killing the running shells.
Because most prompts set a title on every prompt, an array-based model tore
down the terminal as soon as you ran any command. `setTabTitle()` uses
`ListModel.setProperty()`, which emits `dataChanged` and leaves the panes
alone.

### Tab layouts

Two tab presentations, switchable with the toolbar's sidebar button or View >
Thumbnail Sidebar (`Ctrl+Shift+B`), in the settings dialog, or by setting
`appearance/tabLayout`:

- **standard** — tabs along the top (the default).
- **thumbnails** — a left sidebar of cards, each with a live preview of that
  terminal.

Each card shows a live preview of the tab, its title, and a second line saying
what it is: the program running (when it isn't just the shell), the directory,
and how many panes, or for tmux the active pane's title, with a `tmux <session>`
badge on the preview. A dot marks a tab that has had output you haven't seen.

- **Click** a card to switch to it; **drag** cards to reorder the tabs (the tab
  bar follows, and the other way round); **middle-click**, or the ✕ that
  appears on hover, closes it. Right-click for **Rename…**, **Close Tab**,
  **Close Other Tabs** and **New Tab**. **New Tab** is also at the foot of the
  list.
- **File > Rename Tab…** names a tab in either layout. Leave it empty to follow
  the terminal's title again; on a tmux tab it renames the tmux window.
- **The sidebar's width** is set by dragging its edge and is remembered; cards
  and previews scale with it, keeping the terminal area's shape. Dragging it into
  the window edge hides it (as `Ctrl+Shift+B` does), and double-clicking the edge
  resets the width.
- **Colours come from the terminal scheme**: the sidebar is the current tab's
  background lifted slightly, so it reads as part of the terminal.

Previews are drawn from each pane's screen contents — the emulator's character
image — not grabbed from the widget (patch 8 in
`third_party/qtermwidget/SUPERTERM.md`). A hidden `QTabWidget` page never
paints, so grabbing it gives nothing; drawing from the screen works for hidden
tabs as well as the visible one, with no need to bring them to the front. A
split tab previews as its arrangement of panes. Every 250 ms, the tabs that have
had output since their last preview are redrawn (about 2 ms for a full screen);
idle tabs cost nothing.

### Box-drawing, block and Powerline characters

Some characters are painted as **geometry** sized to the cell rather than taken
from the font, so they stay grid-aligned and join their neighbours at any point
size. A font's glyphs for these are sized to its own ascent and descent, not to
the whole-pixel cell, which leaves hairline gaps at many sizes.

- **Block elements** (U+2580–U+259F: `▀▄█▌▐░▒▓▖▗▘▙▚▛▜▝▞▟` …) and **Powerline
  separators** (U+E0B0–U+E0BF) are always drawn this way. This is a patch to
  the vendored QTermWidget.
- **Box drawing** (U+2500–U+257F) is drawn this way when
  `QTermWidget::setDrawLineChars()` is on (Konsole calls this "use own line
  drawing characters"). The **Scrolling → line-drawing** setting toggles it. It
  defaults to on because that is also the library's own default; turn it off to
  use the font's glyphs instead. (Before this was exposed the app called
  `setDrawLineChars(true)` on a display that was already `true`, which was a
  no-op.)

The practical consequence: line and block art looks right regardless of your
font size. Symbols wider than a cell (often from a fallback font) are no longer
clipped to the cell; a glyph whose advance is two cells wide is centred on its
one cell instead.

### Watching a terminal for activity or silence

Settings → Scrolling has a small "Watch a terminal" group. Both options mark a
non-current tab with the same `●` dot that output already uses, clearing when
you select the tab — so they are a cue, not a popup.

- **Output arrives** — mark the tab the first time output appears. The
  counterpart to the existing busy marker, useful when you're waiting on
  something in a background tab and want to know the moment it stirs.
- **Output stops** — mark the tab when it has been quiet for the configured
  number of seconds (default 10). The classic "build finished" / "test suite
  is done" cue: start the command, switch away, and the dot tells you when it
  has gone idle.

Both live on top of QTermWidget's `setMonitorActivity()` / `setMonitorSilence()`
/ `setSilenceTimeout()` and are off by default, since in an interactive shell
where you're typing, "no output for N seconds" is the normal case rather than
a signal.

## Colour scheme editor

**Settings → Appearance → Colour scheme → Edit…** opens an editor with a live
preview and all 20 colours a Konsole-format scheme defines:

| Group | Roles |
| --- | --- |
| Text and background | `Foreground`, `Background` |
| ANSI colours | `Color0`–`Color7` (black, red, green, yellow, blue, magenta, cyan, white) |
| ANSI colours (bright) | `Color0Intense`–`Color7Intense` |
| — | `ForegroundIntense`, `BackgroundIntense` (used for bold text) |

The last two are easy to miss, since the "intense" set looks like it should be
only the eight ANSI colours: leaving them out makes bold text render black.

**Your own schemes are edited in place; built-in ones are copied.** The editor
seeds itself from the selected scheme's colours and decides from where that
scheme came:

- a scheme **you** saved (`~/.local/share/superterm/color-schemes/`) opens under
  its own name and saving updates it — so adjusting the background of a theme
  you built does not spawn `<name> copy`
- a **built-in** scheme opens as `<scheme> copy`, so a bundled preset can never
  be overwritten

Renaming the copy in either mode saves under the new name instead. **Reset
Colours** re-reads the original seed.

Saved schemes land in:

```
~/.local/share/superterm/color-schemes/<name>.colorscheme
```

That directory is registered with qtermwidget at startup, so your schemes appear
alongside the bundled ones everywhere a colour scheme is chosen — including the
profile editor.

### File format

The editor writes the standard Konsole `.colorscheme` INI file, which you can
also hand-edit:

```ini
[General]
Description=Nord
Opacity=1
Wallpaper=

[Background]
Color=46,52,64
Transparent=false
```

Two details in that format are worth knowing, because both fail *silently*:

- **Colours must be written unquoted** as `r,g,b`. qtermwidget's parser rejects
  the quoted form and falls back to black, so the editor writes the file
  directly rather than through `QSettings` — which would see a comma value and
  emit `Color="46,52,64"`.
- **`[General]` is QSettings' implicit root group**, so its keys land at top
  level. Reading it via `beginGroup("General")` yields nothing; qtermwidget
  itself does this, which is why a scheme's description reads as empty. Only
  the description is affected, so it is harmless — the editor reads the file
  with its own parser and shows it correctly.

qtermwidget caches its scheme list the first time it is asked, and it keys
loaded schemes by file *basename*. Three consequences are handled in the code:
every scheme dropdown is built from `ColorSchemeFile::allSchemeNames()`, which
merges in the user scheme directory — the library's own list alone silently
omits custom schemes; the dropdowns are rebuilt by hand after a save; and
schemes are applied **by full path**. Applying a name the process has not loaded
silently falls back to the *default* scheme rather than reporting an error.

### Why editing in place writes a temporary file

The basename keying has a further consequence: a scheme already loaded **cannot
be reloaded**. `loadColorScheme()` discards a freshly parsed scheme whose name is
known and `findColorScheme()` keeps returning the cached instance, so re-saving
`MyTheme` would leave every open terminal showing the *old* colours until
restart.

To make an in-place edit take effect immediately, the editor applies a copy of
the scheme under a throwaway name (`_live_<timestamp>_<n>.colorscheme`), which
the cache has not seen. That is transparent:

- the temporary file is deleted as soon as it is superseded, and any left behind
  by a crash is cleaned up at startup
- it never appears in a scheme dropdown
- the config keeps storing your real scheme name, so the next launch resolves
  back to `MyTheme`
- the status bar shows the real name, not the throwaway one

## Profiles

A **profile** is a named bundle of per-terminal overrides: a colour scheme, a
font, a shell program and its arguments, and a start directory. Profiles let
one window hold, say, a themed "Dev" tab running `fish -l` next to a plain
default tab.

There is always exactly one **default** profile, and that is what `New Tab`
(`Ctrl+Shift+T`) opens. To open a tab with a different profile, use
**File → New Tab with Profile** and pick one from the list.

Profiles are managed in **Settings → Profiles**: a list on the left with add,
duplicate and delete buttons under it, and a form on the right. The default
profile is starred in the list. To make another profile the default, tick
**Open new tabs with this profile** on it.

### Inheritance

Every field is independent, and an unset field **inherits the global setting**:

| Field left at *Inherit* | Follows |
| --- | --- |
| Colour scheme | the Appearance page |
| Font / font size | the Appearance page |
| Shell program | the login shell (`$SHELL`) |
| Shell arguments | none |
| Start directory | the home directory |

The distinction matters at runtime. Changing the global font size re-applies
the appearance to every tab, and only the fields a profile actually overrides
stay put — so a "themed" profile keeps its colour scheme while still tracking
your global font size. Resolution happens on every apply, not once at creation.

### What profiles deliberately do *not* do

- **They do not restart a running shell.** Editing a profile's shell program
  updates the appearance of open tabs immediately, but their shells keep
  running; the new program takes effect on the next tab opened with that
  profile. Relaunching would throw away whatever is in the tab.
- **Deleting a profile does not affect open tabs.** They keep the settings they
  resolved with, and are not silently reassigned to the default.
- **The default profile cannot be deleted** — it is what `New Tab` opens. It can
  be renamed and given overrides like any other, and the default can be moved
  to another profile.

`SUPERTERM_SHELL` still wins over a profile's shell program, since it is an
explicit per-run instruction:

```sh
SUPERTERM_SHELL=/bin/fish superterm   # every new tab uses fish
```

Config layout, under `[profiles]`:

```ini
[profiles]
order=5a907420, 844949ed
default=5a907420
5a907420\name=Default
844949ed\name=Dev
844949ed\colorScheme=Nord
844949ed\fontSize=18
844949ed\shellProgram=/usr/bin/fish
844949ed\shellArgs=-l
844949ed\workingDirectory=/home/me/work
```

Only fields that are actually set are written, so an inheriting profile stays
short and a later global change still reaches it. Profile ids are stable across
renames, which is what lets open tabs keep pointing at them.

## Toolbar

A toolbar under the menu bar carries the common actions: new tab (the arrow
beside it opens the profile list), close tab, split right and split down, copy,
paste, find, screenshot
(the arrow offers the whole terminal), zoom out and
in, the thumbnail sidebar toggle, and settings at the far end. The current font
size sits between the zoom buttons; click it to reset the zoom.

The buttons are the same actions as the menus, so each tooltip shows its
shortcut. Icons come from the desktop icon theme, with a few alternative names
per action because Breeze and Adwaita name some differently. If the theme has
none of them, the button shows its name instead. **View → Show Toolbar** (or
right-clicking the toolbar) hides it, and its visibility and position are kept
between sessions.

## Split panes

A tab can hold several terminals side by side or stacked. **View → Split
Right** (`Ctrl+Shift+E`) and **Split Down** (`Ctrl+Shift+O`), also on the
toolbar and the right-click menu, put a new terminal beside the current one,
with the same profile and starting in the directory the current shell is in.
Splits nest, and the handles between panes can be dragged.

The pane with keyboard focus is framed in the highlight colour; it is what
copy, paste, find, screenshots and the status bar refer to. Click a pane to
focus it, or cycle with **Next Pane** (`Ctrl+Shift+N`) and **Previous Pane**
(`Ctrl+Shift+M`). **Close Pane** (`Ctrl+Shift+X`) closes one, and so does its
shell exiting; closing the last pane closes the tab. The quit warning checks
every pane, not just the visible one, and a split tab's thumbnail shows the
whole arrangement.

## tmux

The **tmux** menu attaches to tmux sessions in **control mode**, the way
iTerm2 does: each tmux window becomes a tab and each pane a pane, drawn by
superterm rather than by tmux. So scrollback, search, copy, screenshots,
thumbnails and the font all work natively, while the programs live in tmux.
Detach, or quit superterm, and they keep running; attach again (from here or
any tmux client) and everything is where it was, history included.

The menu lists this machine's sessions (choosing one attaches), plus:

- **New Session…** creates one (or attaches, if the name exists).
- **Connect to Host…** does the same on another machine over ssh. ssh has to
  log in without a password prompt (a key or an agent), since there is no
  terminal to type one into.
- **Detach** closes the session's tabs here and leaves it running.

In a tmux tab, the usual actions are carried out by tmux: New Tab creates a
tmux window, Split Right/Down split the pane, Close Pane and Close Tab kill the
pane or window, and dragging a split handle resizes the pane. Closing the
session's last window would end the session, so superterm asks first and
offers to detach instead (the default). Switching tabs
or panes switches tmux's current window and pane too, so another client sees
the same. **File → New Tab with Profile** still opens a local tab. tmux's own
prefix key isn't involved: keys go straight to the program in the pane.

How it works:

- superterm runs `tmux -C` over plain pipes and speaks its line protocol:
  replies to commands, plus notifications such as `%output`,
  `%layout-change` and `%window-add`. Pane output is fed straight into a
  terminal that has no process of its own; keystrokes go back with
  `send-keys -H`.
- tmux decides the layout in character cells. superterm tells tmux how many
  cells fit (`refresh-client -C`) and sizes every pane and split handle to
  whole cells, so each pane shows exactly the grid tmux gave it. That is why
  tmux panes have no scrollbar or per-pane margin: the tab is padded instead.
  Scrolling back works with the wheel and keys.
- On attaching, each pane's history and screen come from `capture-pane`, and
  a full-screen program (an editor, a pager) is put back on the alternate
  screen.
- With tmux 3.2 or later, flow control is on: if superterm falls behind a
  flood of output, tmux pauses that pane, and superterm redraws it from a
  capture when it catches up.
- Both of tmux's layout formats are understood: the classic one in
  `%layout-change`, and the JSON one tmux 3.8 uses in format variables.

Tested against tmux 3.8. Connecting over ssh uses the same code path but has
only been tried locally.

## Taking screenshots

**Edit → Screenshot Region…** (`Ctrl+Shift+S`, or the toolbar's camera button)
freezes the terminal and lets you drag out a rectangle. The selection snaps to
whole character cells, so a capture never shows half a glyph; hold **Shift**
while dragging to select freely to the pixel. A label shows the size in cells
(or pixels) as you drag. **Esc** or a right click cancels, as does switching
tab or resizing the window.

**Edit → Screenshot Terminal** (`Ctrl+Shift+P`) captures the whole visible
screen: the character grid and its margin, without the scrollbar.

Either way the image goes straight to the clipboard, and for ten seconds the
status bar offers **Save…**, which suggests
`~/Pictures/superterm-<date>-<time>.png`. Nothing is written to disk unless you
ask. Both actions are also in the terminal's right-click menu.

Captures are at device resolution, so on a scaled display they come out at the
real pixel size rather than the logical one.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+T` | New tab |
| `Ctrl+Shift+W` | Close tab |
| `Ctrl+Shift+E` / `Ctrl+Shift+O` | Split right / down |
| `Ctrl+Shift+X` | Close pane |
| `Ctrl+Shift+N` / `Ctrl+Shift+M` | Next / previous pane |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous tab |
| `Ctrl+Shift+F` | Find in terminal |
| `Ctrl+Shift+S` | Screenshot a region (drag to select) |
| `Ctrl+Shift+P` | Screenshot the whole terminal |
| `Ctrl+,` | Open settings |
| `Ctrl+=` / `Ctrl+-` | Zoom in / out (0.5pt steps) |
| `Ctrl+0` | Reset zoom |
| `Ctrl+Shift+B` | Toggle thumbnail sidebar |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy / paste |
| `Ctrl+Shift+Q` | Quit |
| `Ctrl` + mouse wheel | Zoom |
| `Escape` | Close search or settings |

Right-clicking inside a terminal opens a context menu with copy, paste, find,
clear, split, close-pane, screenshot, save-output-to-file, and zoom actions, so those are discoverable without
memorising the shortcuts.

Plain `Ctrl`+letter combinations are deliberately **not** bound to app actions.
A `QAction` shortcut is claimed by the shortcut map before the focused widget —
the terminal — ever sees the key, so binding one would silently break the
program running inside it. `Ctrl+Q` in particular is micro's quit key, and the
shell's XON resume; it used to be `Quit` (via `QKeySequence::Quit`) and killed
SuperTerm instead of the editor. Quit is now `Ctrl+Shift+Q`.

The plain-`Ctrl` bindings that remain (`Ctrl+Tab`, `Ctrl+,`, `Ctrl+=`/`-`/`0`)
are non-letter keys that produce no control code, so no terminal program wants
them. Anything else uses `Ctrl+Shift`+letter.

## Project layout

The application is the Qt Widgets build. The original Qt Quick implementation
is kept buildable but is no longer installed.

```
CMakeLists.txt            build definition
src-widgets/main.cpp      QApplication bootstrap, style and icon
src-widgets/mainwindow.*  tabs, menus, toolbar, status bar, preview rotation
src-widgets/terminaltab.*  one tab: a tree of panes in splitters
src-widgets/terminalwidget.*  one pane: a QTermWidget plus appearance plumbing
src-widgets/tmuxcontrol.*  tmux control-mode protocol client
src-widgets/tmuxsession.*  a tmux session's windows and panes as tabs and panes
src-widgets/settingsdialog.*  modal settings, four sections
src-widgets/profiles.*    terminal profiles and their persistence
src-widgets/colorscheme.* .colorscheme file read/write (20 roles)
src-widgets/colorschemeeditor.*  scheme editor dialog with live preview
src-widgets/screenshotoverlay.*  drag-to-select region screenshots
src-widgets/thumbnailsidebar.*  left sidebar of tab cards with live previews
src-widgets/settings.*    QSettings-backed configuration
third_party/qtermwidget/  vendored, patched QTermWidget (see SUPERTERM.md)
data/superterm.desktop    launcher entry
icons/superterm.svg       app icon

qml/, src/                legacy Qt Quick build (optional, see below)
```

### Building the legacy QML version

```sh
cmake -B build -DSUPERTERM_BUILD_QML=ON
cmake --build build --target superterm-qml
```

It requires the separate QMLTermWidget QML module and is not installed. The two
builds share the same config file, so settings carry across.
## Desktop integration

**Colour scheme.** On a Plasma session the KDE platform theme supplies a
palette to Qt, so the chrome takes the user's actual Plasma colours and follows
live changes — switching Plasma between light and dark restyles the tab bar and
status bar immediately, with no restart.

Worth knowing: this comes from the *platform theme*, not the Qt Quick style.
Fusion reports exactly the same `SystemPalette` values as KDE's own styles, so
the colour scheme is independent of which Quick style is in use.

**Controls style.** On Plasma the app prefers `org.kde.union`, Plasma's own
style (the `union` package), which covers both QtWidgets and QtQuick. Otherwise
it uses Fusion.

`org.kde.desktop` is deliberately *not* used. In `qqc2-desktop-style` 6.31.0 it
segfaults the moment a `ComboBox` is instantiated, and since Fusion already
yields the identical palette it would buy nothing but risk. To force a style:

```sh
SUPERTERM_QUICK_STYLE=Fusion ./build/superterm    # or Basic, Material, ...
SUPERTERM_SHOWSTYLE=1 ./build/superterm           # log the choice
```

The terminal itself always paints its own colour scheme (Solarized by
default), so chrome and terminal stay visually distinct.

## Platform support

**Linux, macOS and the BSDs work.** Windows does not, yet — see below.

| Platform | Status |
| --- | --- |
| Linux / X11 / Wayland | ✅ tested |
| macOS | ✅ expected (QMLTermWidget supports it) |
| FreeBSD et al. | ✅ expected |
| Windows | ❌ no pty backend |

### Why not Windows

Both QMLTermWidget and its upstream [lxqt/qtermwidget](https://github.com/lxqt/qtermwidget)
are **POSIX-pty only**. Their terminal session is built on a vendored KDE KPty,
which calls `openpty()`, `posix_openpt()` and `termios` directly. There is no
ConPTY or winpty code path in either project, so the emulator cannot attach to
a Windows pseudo-console.

Supporting Windows means supplying a different session backend behind the same
QML surface: a ConPTY-backed session object exposing the `QMLTermSession` API
(`shellProgram`, `initialWorkingDirectory`, `startShellProgram()`,
`sendText()`, the `finished`/`titleChanged` signals, and `history`/`currentDir`).
`TerminalPane.qml` is the only file that would need to change. That is real
work, and it is not implemented here.

## Implementation notes

A few things that cost time and are worth knowing if you extend this:

- **The QML type names are not the C++ names.** The display is `QMLTermWidget`
  (C++ class `Konsole::TerminalDisplay`) and the session is `QMLTermSession`
  (C++ class `KSession`). `TerminalDisplay` / `TerminalSession` do not exist as
  QML types.
- **Do not link `qtermwidget6`** (the Qt Widgets library) alongside the QML
  plugin. The plugin vendors its own copy of the Konsole/KPty sources, so
  linking the Widgets library pulls in a second copy of the same symbols. The
  symptom is ugly: the pane paints its background and cursor but **no text**,
  and the app aborts on exit with `corrupted double-linked list`. Only put the
  plugin's module directory on the import path.
- **Start the shell after the display has a size.** Launching from
  `Component.onCompleted` races layout; the pane can end up showing a bare
  background. `TerminalPane.qml` waits for non-zero width/height.
- **Re-focus the terminal on every tab change.** Keystrokes go to the item
  holding active focus. Changing the current tab takes the old item out of the
  focus chain and leaves focus on the root item, so typing stops working in
  *every* tab. `Main.qml` calls `focusCurrentTab()` whenever the tab index
  changes. The same bug silences the cursor blink, because QMLTermWidget only
  starts its blink timer when the display has focus.
- **Chrome buttons must set `focusPolicy: Qt.NoFocus`.** A `ToolButton` takes
  active focus when clicked, so pressing the gear button silently stole the
  keyboard from the terminal and it stopped responding until you switched
  tabs. `activeFocusOnTab: false` is *not* enough — it only affects tab
  navigation, not mouse focus. The drawer also returns focus to the terminal
  when it closes, since the sliders and combos legitimately hold focus while
  in use.
- **Reassign `var` array models instead of mutating them.** `tabModel.push()`
  emits no change signal, so the tab strip and pane `Repeater` never update.
- **`history` is scrollback, not the visible screen** — it stays empty until
  lines scroll off the top.
- `currentDir` and `hasActiveProcess` have no `NOTIFY` signal, so the status bar
  polls them once a second.

## Licence

GPL-2.0-or-later, to match QMLTermWidget (which is GPL-2.0-or-later, inherited
from QTermWidget).

## Qt gotchas worth knowing

- `QtObject` has no default property, so a `SystemPalette` cannot be declared
  inside a plain `QtObject` singleton. `Theme.qml` is an invisible
  `Item` for that reason.
- `QQuickStyle` exposes no import-path query API, so detecting whether
  `org.kde.desktop` is installed means probing `QML2_IMPORT_PATH` plus
  `QLibraryInfo::QmlImportsPath` for `org/kde/desktop/qmldir`.
- `QLibraryInfo` enumerates `QmlImportsPath`, not `QmlImports`.
- Window position is set by assigning `x`/`y`; there is no `setPosition()`.
- **`ComboBox` crashes `org.kde.desktop`.** `qqc2-desktop-style` 6.31.0
  segfaults as soon as a `ComboBox` is instantiated — reproduced in a bare
  probe app with `QQuickStyle::setStyle("org.kde.desktop")` and a single
  `ComboBox` in a `ColumnLayout`, independent of the Qt *widget* theme.
  `Fusion`, `Basic` and `org.kde.union` are all unaffected. The app therefore
  selects Union rather than `org.kde.desktop`; if you override the style with
  `SUPERTERM_QUICK_STYLE=org.kde.desktop` the settings drawer will crash.
