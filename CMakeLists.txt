cmake_minimum_required(VERSION 3.21)

project(puddlejumper
    VERSION 0.2.0
    DESCRIPTION "A modern terminal built on Qt Widgets and QTermWidget"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Qt Widgets build is the application. QtQuick is only needed for the
# legacy QML implementation, which is optional (see PUDDLEJUMPER_BUILD_QML below).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg Network)
qt_standard_project_setup(REQUIRES 6.5)

include(GNUInstallDirs)

# QTermWidget is vendored and patched (third_party/qtermwidget/PUDDLEJUMPER.md),
# so the system qtermwidget6 package is not used.
add_subdirectory(third_party/qtermwidget EXCLUDE_FROM_ALL)

# Workspace files are YAML, in smug's format.
find_package(yaml-cpp 0.7 REQUIRED)
find_package(ZLIB REQUIRED)   # VNC's compressed updates

# RDP tabs, where FreeRDP 3 is to be had (a spike, so far).
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FREERDP IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)
endif()

# The UI's icons: Lucide (ISC, see icons/lucide/LICENSE), compiled in and
# drawn in the palette's text colour by src-widgets/appicon.cpp.
file(GLOB LUCIDE_ICONS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" icons/lucide/*.svg)

# ---------------------------------------------------------------------------
# puddlejumper — the application.
#
# Built on a vendored copy of qtermwidget (Qt Widgets). This is the supported
# build; it is the one that gets installed, and the one the desktop entry
# launches.
# ---------------------------------------------------------------------------
qt_add_executable(puddlejumper
    src-widgets/aboutdialog.cpp
    src-widgets/aboutdialog.h
    src-widgets/popupframe.h
    src-widgets/appicon.cpp
    src-widgets/appicon.h
    src-widgets/cardstyle.h
    src-widgets/disclaim.cpp
    src-widgets/disclaim.h
    src-widgets/formername.cpp
    src-widgets/formername.h
    src-widgets/macwindow.h
    src-widgets/main.cpp
    src-widgets/mainwindow.cpp
    src-widgets/rexactions.cpp
    src-widgets/rexprompt.cpp src-widgets/rexprompt.h
    src-widgets/commandpalette.cpp src-widgets/commandpalette.h
    src-widgets/palette.cpp src-widgets/palette.h
    src-widgets/mainwindow.h
    src-widgets/notifier.cpp
    src-widgets/notifier.h
    src-widgets/programstatus.cpp
    src-widgets/programstatus.h
    src-widgets/programresume.cpp src-widgets/programresume.h
    src-widgets/resumerules.cpp src-widgets/resumerules.h
    src-widgets/settingsdialog.cpp
    src-widgets/settingsdialog.h
    src-widgets/tab.h
    src-widgets/terminaltab.cpp
    src-widgets/terminaltab.h
    src-widgets/terminalwidget.cpp
    src-widgets/terminalwidget.h
    src-widgets/thumbnailsidebar.cpp
    src-widgets/thumbnailsidebar.h
    src-widgets/settings.cpp
    src-widgets/settings.h
    src-widgets/profiles.cpp
    src-widgets/profiles.h
    src-widgets/colorscheme.cpp
    src-widgets/colorscheme.h
    src-widgets/colorschemeeditor.cpp
    src-widgets/colorschemeeditor.h
    src-widgets/screenshotoverlay.cpp
    src-widgets/screenshotoverlay.h
    src-widgets/tabstrip.cpp
    src-widgets/flattoolbutton.cpp
    src-widgets/flattoolbutton.h
    src-widgets/tabstrip.h
    src-widgets/procinfo.cpp
    src-widgets/procinfo.h
    src-widgets/tmuxcontrol.cpp
    src-widgets/tmuxcontrol.h
    src-widgets/tmuxsession.cpp
    src-widgets/tmuxsession.h
    src-widgets/tmuxworkspace.cpp
    src-widgets/tmuxworkspace.h
    src-widgets/vncclient.cpp
    src-widgets/vncclient.h
    src-widgets/serverstore.cpp
    src-widgets/serverstore.h
    src-widgets/remoteclient.h
    src-widgets/vnccrypto.cpp
    src-widgets/vnccrypto.h
    src-widgets/appleadaptive.cpp
    src-widgets/appleadaptive.h
    src-widgets/applepasteboard.cpp
    src-widgets/applepasteboard.h
    src-widgets/vncview.cpp
    src-widgets/vncview.h
    src-widgets/vnctab.cpp
    src-widgets/vnctab.h
    src-widgets/workspace.cpp
    src-widgets/workspace.h
    src-widgets/workspacefile.cpp
    src-widgets/workspacefile.h
    src-widgets/wsl.cpp
    src-widgets/wsl.h
)

target_link_libraries(puddlejumper PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
    qtermwidget
    yaml-cpp::yaml-cpp
    ZLIB::ZLIB
)

set_target_properties(puddlejumper PROPERTIES AUTOMOC ON)

# macOS: the toolbar in the title bar (src-widgets/macwindow.h), and an app
# bundle, build/PuddleJumper.app. packaging/macos/build-app.sh makes it a
# self-contained one, in a disk image.
if(APPLE)
    enable_language(OBJCXX)
    target_sources(puddlejumper PRIVATE src-widgets/macwindow.mm src-widgets/notifier_mac.mm)
    target_link_libraries(puddlejumper PRIVATE "-framework AppKit" "-framework UserNotifications")

    set(PUDDLEJUMPER_ICNS icons/puddlejumper.icns)
    set_source_files_properties(${PUDDLEJUMPER_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(puddlejumper PRIVATE ${PUDDLEJUMPER_ICNS})
    set_target_properties(puddlejumper PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME PuddleJumper
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/data/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME PuddleJumper
        MACOSX_BUNDLE_GUI_IDENTIFIER com.puddlejumper.puddlejumper   # as the settings' domain
        MACOSX_BUNDLE_ICON_FILE puddlejumper.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_COPYRIGHT "GNU General Public License v2 or later"
    )
endif()

# Desktop notifications go over D-Bus on Linux and the BSDs (notifier.cpp).
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS DBus)
    if(TARGET Qt6::DBus)
        target_compile_definitions(puddlejumper PRIVATE PUDDLEJUMPER_DBUS)
        target_link_libraries(puddlejumper PRIVATE Qt6::DBus)
    endif()
endif()

# Rex tabs: sessions on a Rex server, over Qt WebSockets.
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS WebSockets)
if(TARGET Qt6::WebSockets)
    message(STATUS "Rex tabs: on")
    target_sources(puddlejumper PRIVATE
        src-widgets/rexsession.cpp src-widgets/rexsession.h
        src-widgets/rexoffline.cpp src-widgets/rexoffline.h
        src-widgets/rexkeymap.cpp src-widgets/rexkeymap.h
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexfilecopy.cpp src-widgets/rexfilecopy.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h)
    target_compile_definitions(puddlejumper PRIVATE PUDDLEJUMPER_REX)
    target_link_libraries(puddlejumper PRIVATE Qt6::WebSockets)
else()
    message(STATUS "Rex tabs: off (no Qt WebSockets)")
endif()

# Windows: a GUI program (no console window of its own), PuddleJumper.exe.
# packaging/windows/build-app.sh makes a self-contained folder of it, zipped.
if(WIN32)
    target_sources(puddlejumper PRIVATE data/windows/puddlejumper.rc)
    set_target_properties(puddlejumper PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME PuddleJumper
    )
endif()

# Embed the icon so the window and taskbar get it without an install step.
qt_add_resources(puddlejumper "app_icons"
    PREFIX "/"
    FILES icons/puddlejumper.svg icons/portal-prompt.svg
)
# The version and the commit built from, for About (and TERM_PROGRAM_VERSION).
# Configured again after each commit, so the commit stays current.
find_package(Git QUIET)
set(PUDDLEJUMPER_GIT_COMMIT "")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE PUDDLEJUMPER_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/logs/HEAD")
    endif()
endif()
target_compile_definitions(puddlejumper PRIVATE
    PUDDLEJUMPER_VERSION="${PROJECT_VERSION}"
    PUDDLEJUMPER_GIT_COMMIT="${PUDDLEJUMPER_GIT_COMMIT}")
qt_add_resources(puddlejumper "lucide_icons"
    PREFIX "/lucide"
    BASE icons/lucide
    FILES ${LUCIDE_ICONS}
)
# The tabs' protocol icons (terminal, tmux, Rex, VNC, RDP), in currentColor.
file(GLOB CONNECTION_ICONS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" icons/connections/*.svg)
qt_add_resources(puddlejumper "connection_icons"
    PREFIX "/connections"
    BASE icons/connections
    FILES ${CONNECTION_ICONS}
)

install(TARGETS puddlejumper
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Desktop entry and icon, so the app appears in launchers and the xdg portal
# recognises its app id (which also silences the startup portal warning).
if(UNIX AND NOT APPLE)
    install(FILES data/puddlejumper.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES icons/puddlejumper.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )
endif()

# ---------------------------------------------------------------------------
# probe-vnc — drives the VNC view against a live server. Dev tool only; never
# installed. Run: QT_QPA_PLATFORM=offscreen ./build/probe-vnc localhost:5900 /tmp/vnc
# ---------------------------------------------------------------------------
qt_add_executable(probe-vnc
    src-widgets/dev/probe_vnc.cpp
    src-widgets/vnccrypto.cpp src-widgets/vnccrypto.h
    src-widgets/appleadaptive.cpp src-widgets/appleadaptive.h
    src-widgets/applepasteboard.cpp src-widgets/applepasteboard.h
    src-widgets/vncclient.cpp src-widgets/vncclient.h
    src-widgets/vncview.cpp src-widgets/vncview.h
    src-widgets/remoteclient.h
)
target_link_libraries(probe-vnc PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    ZLIB::ZLIB
)
set_target_properties(probe-vnc PROPERTIES AUTOMOC ON)

# Web tabs, on Qt WebEngine (Chromium) where it is there to build with; it
# isn't for MinGW, so the Windows build goes without.
option(PUDDLEJUMPER_WEB "Web tabs (needs Qt WebEngine)" ON)
if(PUDDLEJUMPER_WEB)
    find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS WebEngineWidgets)
endif()
if(PUDDLEJUMPER_WEB AND TARGET Qt6::WebEngineWidgets)
    message(STATUS "Web tabs: on")
    target_sources(puddlejumper PRIVATE src-widgets/webtab.cpp src-widgets/webtab.h)
    target_compile_definitions(puddlejumper PRIVATE PUDDLEJUMPER_WEB)
    target_link_libraries(puddlejumper PRIVATE Qt6::WebEngineWidgets)
    # WebEngine brings Qt QML along, and with it a scan of the sources for QML
    # imports, which would find the legacy QML version's.
    set_target_properties(puddlejumper PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
else()
    message(STATUS "Web tabs: off (no Qt WebEngine)")
endif()

foreach(target puddlejumper probe-vnc)
    if(FREERDP_FOUND)
        target_sources(${target} PRIVATE src-widgets/rdpclient.cpp src-widgets/rdpclient.h)
        target_compile_definitions(${target} PRIVATE PUDDLEJUMPER_RDP)
        target_link_libraries(${target} PRIVATE PkgConfig::FREERDP)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# probe-settingsdialog — offscreen diagnostics for the Settings dialog. Dev
# tool only; never installed. Run: QT_QPA_PLATFORM=offscreen ./build/probe-settingsdialog
# ---------------------------------------------------------------------------
qt_add_executable(probe-settingsdialog
    src-widgets/dev/probe_settingsdialog.cpp
    src-widgets/appicon.cpp src-widgets/appicon.h
    src-widgets/settingsdialog.cpp src-widgets/settingsdialog.h
    src-widgets/settings.cpp src-widgets/settings.h
    src-widgets/serverstore.cpp src-widgets/serverstore.h
    src-widgets/profiles.cpp src-widgets/profiles.h
    src-widgets/colorscheme.cpp src-widgets/colorscheme.h
    src-widgets/colorschemeeditor.cpp src-widgets/colorschemeeditor.h
)
target_link_libraries(probe-settingsdialog PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    qtermwidget
)
qt_add_resources(probe-settingsdialog "lucide_icons"
    PREFIX "/lucide"
    BASE icons/lucide
    FILES ${LUCIDE_ICONS}
)
set_target_properties(probe-settingsdialog PROPERTIES AUTOMOC ON)
# Probe sources live under src-widgets/dev, so give it access to the parent
# headers without "../..".
target_include_directories(probe-settingsdialog PRIVATE src-widgets)
target_compile_definitions(probe-settingsdialog PRIVATE PUDDLEJUMPER_REX)  # its Rex settings too

# ---------------------------------------------------------------------------
# probe-tabbars — draws the tab strip and the sidebar's header (workspace and
# new-tab buttons at rest, hovered, pressed) in the platform's style. Dev tool
# only; never installed. Run: ./build/probe-tabbars OUTDIR [STYLE]
# ---------------------------------------------------------------------------
qt_add_executable(probe-tabbars
    src-widgets/dev/probe_tabbars.cpp
    src-widgets/tabstrip.cpp src-widgets/tabstrip.h
    src-widgets/flattoolbutton.cpp src-widgets/flattoolbutton.h
    src-widgets/thumbnailsidebar.cpp src-widgets/thumbnailsidebar.h
    src-widgets/appicon.cpp src-widgets/appicon.h
    src-widgets/cardstyle.h
)
target_link_libraries(probe-tabbars PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
qt_add_resources(probe-tabbars "lucide_icons"
    PREFIX "/lucide"
    BASE icons/lucide
    FILES ${LUCIDE_ICONS}
)
set_target_properties(probe-tabbars PROPERTIES AUTOMOC ON)
target_include_directories(probe-tabbars PRIVATE src-widgets)

# ---------------------------------------------------------------------------
# test-programstatus — the OSC 7501 parser and records against the spec's
# rules. Dev tool only; never installed. Run: ./build/test-programstatus
# ---------------------------------------------------------------------------
qt_add_executable(test-programstatus
    src-widgets/dev/test_programstatus.cpp
    src-widgets/programstatus.cpp src-widgets/programstatus.h
)
target_link_libraries(test-programstatus PRIVATE Qt6::Core)
set_target_properties(test-programstatus PROPERTIES AUTOMOC ON)
target_include_directories(test-programstatus PRIVATE src-widgets)

# ---------------------------------------------------------------------------
# test-programresume — the OSC 7577 parser and the trust rules, against
# atlantis's cases. Dev tool only; never installed. Run:
# ./build/test-programresume
# ---------------------------------------------------------------------------
qt_add_executable(test-programresume
    src-widgets/dev/test_programresume.cpp
    src-widgets/programresume.cpp src-widgets/programresume.h
    src-widgets/resumerules.cpp src-widgets/resumerules.h
    src-widgets/procinfo.cpp src-widgets/procinfo.h
)
target_link_libraries(test-programresume PRIVATE Qt6::Core)
target_include_directories(test-programresume PRIVATE src-widgets)

# ---------------------------------------------------------------------------
# test-serverstore — saved servers through QSettings and back. Dev tool only;
# never installed. Run: ./build/test-serverstore
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS Test)
qt_add_executable(test-serverstore
    src-widgets/dev/test_serverstore.cpp
    src-widgets/serverstore.cpp src-widgets/serverstore.h
)
target_link_libraries(test-serverstore PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test-serverstore PRIVATE src-widgets)

# ---------------------------------------------------------------------------
# test-ghosttysnapshot — the Ghostty snapshot decoder against Ghostty's golden
# fixtures (src-widgets/dev/ghostty-snapshot). Dev tool only; never installed.
# Run: ./build/test-ghosttysnapshot
# ---------------------------------------------------------------------------
qt_add_executable(test-ghosttysnapshot
    src-widgets/dev/test_ghosttysnapshot.cpp
    src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
)
target_link_libraries(test-ghosttysnapshot PRIVATE Qt6::Core)
target_include_directories(test-ghosttysnapshot PRIVATE src-widgets)
target_compile_definitions(test-ghosttysnapshot PRIVATE
    PJ_GHOSTTY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/src-widgets/dev/ghostty-snapshot")

# test-ghosttyreplay — snapshots replayed into qtermwidget's emulator (no
# window) and compared with what they hold. Dev tool only; never installed.
# Run: ./build/test-ghosttyreplay
qt_add_executable(test-ghosttyreplay
    src-widgets/dev/test_ghosttyreplay.cpp
    src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
    src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
)
target_link_libraries(test-ghosttyreplay PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets qtermwidget)
target_include_directories(test-ghosttyreplay PRIVATE src-widgets)
target_compile_definitions(test-ghosttyreplay PRIVATE
    PJ_GHOSTTY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/src-widgets/dev/ghostty-snapshot")

# test-clipboard — programs' clipboard writes (OSC 52, kitty's OSC 5522) in
# qtermwidget's emulator. Dev tool only; never installed.
# Run: ./build/test-clipboard (offscreen; QT_QPA_PLATFORM=wayland for a real one)
qt_add_executable(test-clipboard src-widgets/dev/test_clipboard.cpp)
target_link_libraries(test-clipboard PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets qtermwidget)
target_include_directories(test-clipboard PRIVATE src-widgets)

# test-graphics — kitty's graphics protocol (TerminalGraphics): chunks as the
# spec and as kitten icat send them, files and shared memory. Dev tool only.
# Run: ./build/test-graphics
qt_add_executable(test-graphics src-widgets/dev/test_graphics.cpp)
target_link_libraries(test-graphics PRIVATE Qt6::Core Qt6::Gui qtermwidget)

# test-rexterminalstream — the Rex terminal stream (snapshot, markers,
# output) on a capture from a Rex-compatible server, and the client path from
# it into qtermwidget. Dev tool only; never installed.
# Run: ./build/test-rexterminalstream
qt_add_executable(test-rexterminalstream
    src-widgets/dev/test_rexterminalstream.cpp
    src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
    src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
    src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
)
target_link_libraries(test-rexterminalstream PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets qtermwidget)
target_include_directories(test-rexterminalstream PRIVATE src-widgets)
target_compile_definitions(test-rexterminalstream PRIVATE
    PJ_REX_CAPTURES="${CMAKE_CURRENT_SOURCE_DIR}/src-widgets/dev/rex-capture")

# test-rexprompt — a Rex prompt (rex.ui.input/pick) and the command palette,
# both palettes: filtering, keys, ticks, answers. Dev tool only; never installed.
# Run: ./build/test-rexprompt [DIR]  (DIR: pictures of each, dark and light)
qt_add_executable(test-rexprompt
    src-widgets/dev/test_rexprompt.cpp
    src-widgets/rexprompt.cpp src-widgets/rexprompt.h
    src-widgets/commandpalette.cpp src-widgets/commandpalette.h
    src-widgets/palette.cpp src-widgets/palette.h
    src-widgets/appicon.cpp src-widgets/appicon.h
)
target_link_libraries(test-rexprompt PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
qt_add_resources(test-rexprompt "lucide_icons"
    PREFIX "/lucide"
    BASE icons/lucide
    FILES ${LUCIDE_ICONS}
)
set_target_properties(test-rexprompt PROPERTIES AUTOMOC ON)
target_include_directories(test-rexprompt PRIVATE src-widgets)

# test-rexkeymap — a Rex keymap: key spellings, lookups, sequences and modes.
# Dev tool only; never installed. Run: ./build/test-rexkeymap
qt_add_executable(test-rexkeymap
    src-widgets/dev/test_rexkeymap.cpp
    src-widgets/rexkeymap.cpp src-widgets/rexkeymap.h
)
target_link_libraries(test-rexkeymap PRIVATE Qt6::Core Qt6::Gui)
target_include_directories(test-rexkeymap PRIVATE src-widgets)

# test-rexclient — the Rex client classes against a running Rex-compatible
# server (tuxosaurus on loopback will do): sessions, the terminal stream,
# resume, resize, exit. Dev tool only; never installed.
# Run: ./build/test-rexclient http://127.0.0.1:51433
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS WebSockets)
if(TARGET Qt6::WebSockets)
    qt_add_executable(test-rexclient
        src-widgets/dev/test_rexclient.cpp
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
    )
    target_link_libraries(test-rexclient PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebSockets qtermwidget)
    target_include_directories(test-rexclient PRIVATE src-widgets)

    # test-rexfilecopy — copying files with a Rex server's fs block: the tar
    # offline, then up and down against a server on this machine.
    # Run: ./build/test-rexfilecopy [http://127.0.0.1:PORT | unix:///path]
    if(UNIX)  # its trees have links and FIFOs in them
    qt_add_executable(test-rexfilecopy
        src-widgets/dev/test_rexfilecopy.cpp
        src-widgets/rexfilecopy.cpp src-widgets/rexfilecopy.h
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
    )
    target_link_libraries(test-rexfilecopy PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebSockets qtermwidget)
    target_include_directories(test-rexfilecopy PRIVATE src-widgets)
    endif()

    # test-rexsession — RexSession (a Rex tab) with real TerminalWidget panes
    # against a running server. Run: ./build/test-rexsession http://127.0.0.1:51433
    qt_add_executable(test-rexsession
        src-widgets/dev/test_rexsession.cpp
        src-widgets/rexsession.cpp src-widgets/rexsession.h
        src-widgets/rexoffline.cpp src-widgets/rexoffline.h
        src-widgets/rexkeymap.cpp src-widgets/rexkeymap.h
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexfilecopy.cpp src-widgets/rexfilecopy.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
        src-widgets/terminalwidget.cpp src-widgets/terminalwidget.h
        src-widgets/terminaltab.cpp src-widgets/terminaltab.h
        src-widgets/appicon.cpp src-widgets/appicon.h
        src-widgets/tab.h
        src-widgets/settings.cpp src-widgets/settings.h
        src-widgets/profiles.cpp src-widgets/profiles.h
        src-widgets/colorscheme.cpp src-widgets/colorscheme.h
        src-widgets/formername.cpp src-widgets/formername.h
        src-widgets/procinfo.cpp src-widgets/procinfo.h
        src-widgets/programstatus.cpp src-widgets/programstatus.h
        src-widgets/programresume.cpp src-widgets/programresume.h
        src-widgets/resumerules.cpp src-widgets/resumerules.h
        src-widgets/screenshotoverlay.cpp src-widgets/screenshotoverlay.h
    )
    target_link_libraries(test-rexsession PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::WebSockets qtermwidget)
    qt_add_resources(test-rexsession "lucide_icons"
        PREFIX "/lucide"
        BASE icons/lucide
        FILES ${LUCIDE_ICONS}
    )
    target_include_directories(test-rexsession PRIVATE src-widgets)

    # test-tmuxsession — a tmux tab on a tmux server of its own (tmux -L):
    # each pane's grid against tmux's size for it, and the margin. Run:
    # ./build/test-tmuxsession
    qt_add_executable(test-tmuxsession
        src-widgets/dev/test_tmuxsession.cpp
        src-widgets/tmuxsession.cpp src-widgets/tmuxsession.h
        src-widgets/tmuxcontrol.cpp src-widgets/tmuxcontrol.h
        src-widgets/disclaim.cpp src-widgets/disclaim.h
        src-widgets/rexsession.cpp src-widgets/rexsession.h
        src-widgets/rexoffline.cpp src-widgets/rexoffline.h
        src-widgets/rexkeymap.cpp src-widgets/rexkeymap.h
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexfilecopy.cpp src-widgets/rexfilecopy.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
        src-widgets/terminalwidget.cpp src-widgets/terminalwidget.h
        src-widgets/terminaltab.cpp src-widgets/terminaltab.h
        src-widgets/appicon.cpp src-widgets/appicon.h
        src-widgets/tab.h
        src-widgets/settings.cpp src-widgets/settings.h
        src-widgets/profiles.cpp src-widgets/profiles.h
        src-widgets/colorscheme.cpp src-widgets/colorscheme.h
        src-widgets/formername.cpp src-widgets/formername.h
        src-widgets/procinfo.cpp src-widgets/procinfo.h
        src-widgets/programstatus.cpp src-widgets/programstatus.h
        src-widgets/programresume.cpp src-widgets/programresume.h
        src-widgets/resumerules.cpp src-widgets/resumerules.h
        src-widgets/screenshotoverlay.cpp src-widgets/screenshotoverlay.h
    )
    target_link_libraries(test-tmuxsession PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::WebSockets qtermwidget)
    qt_add_resources(test-tmuxsession "lucide_icons"
        PREFIX "/lucide"
        BASE icons/lucide
        FILES ${LUCIDE_ICONS}
    )
    target_include_directories(test-tmuxsession PRIVATE src-widgets)

    # test-rexreconnect — a Rex tab whose server stops and starts again (it
    # runs an atlantis of its own on 127.0.0.1:51499). Run:
    # ./build/test-rexreconnect /path/to/atlantis
    qt_add_executable(test-rexreconnect
        src-widgets/dev/test_rexreconnect.cpp
        src-widgets/rexsession.cpp src-widgets/rexsession.h
        src-widgets/rexoffline.cpp src-widgets/rexoffline.h
        src-widgets/rexkeymap.cpp src-widgets/rexkeymap.h
        src-widgets/rexclient.cpp src-widgets/rexclient.h
        src-widgets/rexfilecopy.cpp src-widgets/rexfilecopy.h
        src-widgets/rexsocket.cpp src-widgets/rexsocket.h
        src-widgets/rexterminalstream.cpp src-widgets/rexterminalstream.h
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
        src-widgets/terminalwidget.cpp src-widgets/terminalwidget.h
        src-widgets/terminaltab.cpp src-widgets/terminaltab.h
        src-widgets/appicon.cpp src-widgets/appicon.h
        src-widgets/tab.h
        src-widgets/settings.cpp src-widgets/settings.h
        src-widgets/profiles.cpp src-widgets/profiles.h
        src-widgets/colorscheme.cpp src-widgets/colorscheme.h
        src-widgets/formername.cpp src-widgets/formername.h
        src-widgets/procinfo.cpp src-widgets/procinfo.h
        src-widgets/programstatus.cpp src-widgets/programstatus.h
        src-widgets/programresume.cpp src-widgets/programresume.h
        src-widgets/resumerules.cpp src-widgets/resumerules.h
        src-widgets/screenshotoverlay.cpp src-widgets/screenshotoverlay.h
    )
    target_link_libraries(test-rexreconnect PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::WebSockets qtermwidget)
    qt_add_resources(test-rexreconnect "lucide_icons"
        PREFIX "/lucide"
        BASE icons/lucide
        FILES ${LUCIDE_ICONS}
    )
    target_include_directories(test-rexreconnect PRIVATE src-widgets)

    # test-localresume — program resume (OSC 7577) in real local panes: a
    # report, the rules, and a restore through the helper. Run:
    # ./build/test-localresume
    if(UNIX)
    qt_add_executable(test-localresume
        src-widgets/dev/test_localresume.cpp
        src-widgets/terminalwidget.cpp src-widgets/terminalwidget.h
        src-widgets/appicon.cpp src-widgets/appicon.h
        src-widgets/settings.cpp src-widgets/settings.h
        src-widgets/profiles.cpp src-widgets/profiles.h
        src-widgets/colorscheme.cpp src-widgets/colorscheme.h
        src-widgets/formername.cpp src-widgets/formername.h
        src-widgets/procinfo.cpp src-widgets/procinfo.h
        src-widgets/programstatus.cpp src-widgets/programstatus.h
        src-widgets/programresume.cpp src-widgets/programresume.h
        src-widgets/resumerules.cpp src-widgets/resumerules.h
        src-widgets/screenshotoverlay.cpp src-widgets/screenshotoverlay.h
    )
    target_link_libraries(test-localresume PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg qtermwidget)
    qt_add_resources(test-localresume "lucide_icons"
        PREFIX "/lucide"
        BASE icons/lucide
        FILES ${LUCIDE_ICONS}
    )
    target_include_directories(test-localresume PRIVATE src-widgets)
    endif()
endif()

# With a libghostty-vt build (in a Ghostty checkout: zig build -Demit-lib-vt,
# then -DPJ_GHOSTTY_VT=<checkout>/zig-out), two more dev tools that use a
# real Ghostty terminal:
#   ghostty-snap COLS ROWS < stream > snapshot
#   test-ghosttylive [random streams]: Ghostty's snapshots decoded and
#     replayed into qtermwidget, which must match the stream fed directly.
set(PJ_GHOSTTY_VT "" CACHE PATH "A libghostty-vt build (zig-out), for the snapshot dev tools")
if(PJ_GHOSTTY_VT)
    add_library(pj-ghostty-vt STATIC IMPORTED)
    set_target_properties(pj-ghostty-vt PROPERTIES
        IMPORTED_LOCATION "${PJ_GHOSTTY_VT}/lib/libghostty-vt.a"
        INTERFACE_INCLUDE_DIRECTORIES "${PJ_GHOSTTY_VT}/include"
        INTERFACE_COMPILE_DEFINITIONS GHOSTTY_STATIC)

    qt_add_executable(ghostty-snap src-widgets/dev/ghostty_snap.cpp src-widgets/dev/ghosttyvt.h)
    target_link_libraries(ghostty-snap PRIVATE Qt6::Core pj-ghostty-vt)

    qt_add_executable(test-ghosttylive
        src-widgets/dev/test_ghosttylive.cpp
        src-widgets/ghosttysnapshot.cpp src-widgets/ghosttysnapshot.h
        src-widgets/ghosttyreplay.cpp src-widgets/ghosttyreplay.h
    )
    target_link_libraries(test-ghosttylive PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets qtermwidget pj-ghostty-vt)
    target_include_directories(test-ghosttylive PRIVATE src-widgets)
endif()

# ---------------------------------------------------------------------------
# puddlejumper-qml — the original Qt Quick implementation.
#
# Kept buildable for comparison and as a fallback, but no longer installed and
# no longer the desktop entry target. Off by default: it needs the QMLTermWidget
# QML module, which is a separate (and largely abandoned) dependency.
#
#   cmake -B build -DPUDDLEJUMPER_BUILD_QML=ON
#   cmake --build build --target puddlejumper-qml
# ---------------------------------------------------------------------------
option(PUDDLEJUMPER_BUILD_QML "Also build the legacy Qt Quick implementation" OFF)

if(PUDDLEJUMPER_BUILD_QML)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)

    # QMLTermWidget is a QML plugin (import QMLTermWidget 2.0), NOT a linkable
    # library. Locate its module directory at configure time; main.cpp puts it
    # on the engine's import path.
    #
    # IMPORTANT: do NOT link the QML build against qtermwidget. The plugin
    # vendors its own copy of the Konsole/KPty sources, so linking the Widgets
    # library pulls in a second copy of those symbols: the app then renders
    # blank terminal content and aborts on exit ("corrupted double-linked
    # list"). The plugin .so is self-contained.
    find_path(QMLTERMWIDGET_IMPORT_DIR
        NAMES QMLTermWidget/qmldir
        PATHS
            /usr/lib/qt6/qml
            /usr/lib64/qt6/qml
            /usr/local/lib/qt6/qml
            ${QT6_INSTALL_QML}
    )
    if(NOT QMLTERMWIDGET_IMPORT_DIR)
        message(FATAL_ERROR
            "PUDDLEJUMPER_BUILD_QML=ON but the QMLTermWidget QML module was not found.\n"
            "Install it first, e.g.:\n"
            "  Arch:   pacman -S qmltermwidget\n"
            "  Debian: apt install qmltermwidget-qt6-dev\n"
            "or build https://github.com/Swordfish90/qmltermwidget and point\n"
            "-DQMLTERMWIDGET_IMPORT_DIR=<prefix>/lib/qt6/qml at it.")
    endif()
    message(STATUS "QMLTermWidget import dir: ${QMLTERMWIDGET_IMPORT_DIR}")

    qt_add_executable(puddlejumper-qml
        src/main.cpp
        src/settings_store.h
        src/settings_store.cpp
        src/thumbnail_store.h
        src/thumbnail_store.cpp
        src-widgets/formername.h
        src-widgets/formername.cpp
    )

    # Must be set BEFORE qt_add_qml_module so the generated qmldir marks Theme
    # as a QML singleton.
    set_source_files_properties(qml/Theme.qml
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE
    )

    qt_add_qml_module(puddlejumper-qml
        URI PuddleJumper
        VERSION 1.0
        QML_FILES
            qml/Main.qml
            qml/TerminalPane.qml
            qml/SearchBar.qml
            qml/SettingsPanel.qml
            qml/ThumbnailSidebar.qml
            qml/Theme.qml
        RESOURCES
            icons/puddlejumper.svg
    )

    target_link_libraries(puddlejumper-qml PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
    )

    target_compile_definitions(puddlejumper-qml PRIVATE
        PUDDLEJUMPER_QMLTERMWIDGET_IMPORT_DIR="${QMLTERMWIDGET_IMPORT_DIR}"
    )
endif()
