# SuperTerm

A modern terminal built on **Qt Quick** and **[QMLTermWidget](https://github.com/Swordfish90/qmltermwidget)**.

Tabs, live colour-scheme switching, a monospace font picker, font zoom,
opacity, an inline search bar,
and a keyboard-driven UI — with the terminal emulation itself (VT102/xterm,
Unicode, 256-colour, key bindings) handled by QMLTermWidget's Konsole-derived
engine.

## Screenshots

The window chrome, tab strip, status bar, and a live zsh prompt:

```
┌──────────────────────────────────────────────────────────┐
│  Terminal  ✕                                    ＋   ⚙   │
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
| QMLTermWidget | 2.0 (`import QMLTermWidget 2.0`) |

Install QMLTermWidget:

```sh
# Arch
pacman -S qmltermwidget

# Debian / Ubuntu
apt install qmltermwidget-qt6-dev

# Fedora
dnf install qmltermwidget-qt6
```

If your distro doesn't package it, build it from
[Swordfish90/qmltermwidget](https://github.com/Swordfish90/qmltermwidget)
(`qmake6 && make install`) and point CMake at the module directory:

```sh
cmake -S . -B build -DQMLTERMWIDGET_IMPORT_DIR=/usr/lib/qt6/qml
```

## Build and run

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/superterm
```

Environment overrides, handy for testing:

```sh
SUPERTERM_SHELL=/bin/fish SUPERTERM_SHELL_ARGS="-l" ./build/superterm
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

Appearance settings and window geometry persist across runs via `QSettings`:

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

[window]
x=100
y=80
width=1100
height=700
maximized=false
```

Appearance changes are written as they happen; window geometry is saved when
the window closes. Delete the file to return to defaults.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+Shift+T` | New tab |
| `Ctrl+Shift+W` | Close tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous tab |
| `Ctrl+Shift+F` | Find in terminal |
| `Ctrl+=` / `Ctrl+-` | Zoom in / out |
| `Ctrl+0` | Reset zoom |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy / paste |
| `Ctrl` + mouse wheel | Zoom |
| `Escape` | Close search or settings |

## Project layout

```
CMakeLists.txt          build definition
src/main.cpp            QGuiApplication + QQmlApplicationEngine bootstrap
src/settings_store.*    QSettings wrapper exposed to QML
qml/Main.qml            window chrome, tab strip, status bar, shortcuts
qml/TerminalPane.qml    wraps one QMLTermWidget + QMLTermSession
qml/SearchBar.qml       inline find overlay
qml/SettingsPanel.qml   colour scheme / font / opacity / cursor settings
qml/Theme.qml           singleton: palette, metrics, colour-scheme list
data/superterm.desktop  launcher entry
icons/superterm.svg     app icon
```

## Desktop integration

Under KDE the app detects the session and switches to Qt's native
`org.kde.desktop` QML style (from `qqc2-desktop-style`). The chrome then takes
the user's actual Plasma widget colours and follows live changes — switching
Plasma between light and dark restyles the tab bar and status bar immediately.
Requires `qqc2-desktop-style`; without it the app falls back to Fusion.

Everywhere else it uses Fusion with a fixed dark palette tuned to sit next to
common terminal colour schemes.

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
