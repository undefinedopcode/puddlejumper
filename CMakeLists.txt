cmake_minimum_required(VERSION 3.21)

project(superterm
    VERSION 0.2.0
    DESCRIPTION "A modern terminal built on Qt Widgets and QTermWidget"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Qt Widgets build is the application. QtQuick is only needed for the
# legacy QML implementation, which is optional (see SUPERTERM_BUILD_QML below).
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup(REQUIRES 6.5)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QTERMWIDGET6 REQUIRED IMPORTED_TARGET qtermwidget6)

# ---------------------------------------------------------------------------
# superterm — the application.
#
# Built on qtermwidget6 (Qt Widgets). This is the supported build; it is the
# one that gets installed, and the one the desktop entry launches.
# ---------------------------------------------------------------------------
qt_add_executable(superterm
    src-widgets/main.cpp
    src-widgets/mainwindow.cpp
    src-widgets/mainwindow.h
    src-widgets/settingsdialog.cpp
    src-widgets/settingsdialog.h
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
)

target_link_libraries(superterm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PkgConfig::QTERMWIDGET6
)

set_target_properties(superterm PROPERTIES AUTOMOC ON)

# Embed the icon so the window and taskbar get it without an install step.
qt_add_resources(superterm "app_icons"
    PREFIX "/"
    FILES icons/superterm.svg
)

install(TARGETS superterm
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Desktop entry and icon, so the app appears in launchers and the xdg portal
# recognises its app id (which also silences the startup portal warning).
install(FILES data/superterm.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES icons/superterm.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

# ---------------------------------------------------------------------------
# probe-settingsdialog — offscreen diagnostics for the Settings dialog. Dev
# tool only; never installed. Run: QT_QPA_PLATFORM=offscreen ./build/probe-settingsdialog
# ---------------------------------------------------------------------------
qt_add_executable(probe-settingsdialog
    src-widgets/dev/probe_settingsdialog.cpp
    src-widgets/settingsdialog.cpp src-widgets/settingsdialog.h
    src-widgets/settings.cpp src-widgets/settings.h
    src-widgets/profiles.cpp src-widgets/profiles.h
    src-widgets/colorscheme.cpp src-widgets/colorscheme.h
    src-widgets/colorschemeeditor.cpp src-widgets/colorschemeeditor.h
)
target_link_libraries(probe-settingsdialog PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PkgConfig::QTERMWIDGET6
)
set_target_properties(probe-settingsdialog PROPERTIES AUTOMOC ON)
# Probe sources live under src-widgets/dev, so give it access to the parent
# headers without "../..".
target_include_directories(probe-settingsdialog PRIVATE src-widgets)

# ---------------------------------------------------------------------------
# superterm-qml — the original Qt Quick implementation.
#
# Kept buildable for comparison and as a fallback, but no longer installed and
# no longer the desktop entry target. Off by default: it needs the QMLTermWidget
# QML module, which is a separate (and largely abandoned) dependency.
#
#   cmake -B build -DSUPERTERM_BUILD_QML=ON
#   cmake --build build --target superterm-qml
# ---------------------------------------------------------------------------
option(SUPERTERM_BUILD_QML "Also build the legacy Qt Quick implementation" OFF)

if(SUPERTERM_BUILD_QML)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)

    # QMLTermWidget is a QML plugin (import QMLTermWidget 2.0), NOT a linkable
    # library. Locate its module directory at configure time; main.cpp puts it
    # on the engine's import path.
    #
    # IMPORTANT: do NOT link the QML build against qtermwidget6. The plugin
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
            "SUPERTERM_BUILD_QML=ON but the QMLTermWidget QML module was not found.\n"
            "Install it first, e.g.:\n"
            "  Arch:   pacman -S qmltermwidget\n"
            "  Debian: apt install qmltermwidget-qt6-dev\n"
            "or build https://github.com/Swordfish90/qmltermwidget and point\n"
            "-DQMLTERMWIDGET_IMPORT_DIR=<prefix>/lib/qt6/qml at it.")
    endif()
    message(STATUS "QMLTermWidget import dir: ${QMLTERMWIDGET_IMPORT_DIR}")

    qt_add_executable(superterm-qml
        src/main.cpp
        src/settings_store.h
        src/settings_store.cpp
        src/thumbnail_store.h
        src/thumbnail_store.cpp
    )

    # Must be set BEFORE qt_add_qml_module so the generated qmldir marks Theme
    # as a QML singleton.
    set_source_files_properties(qml/Theme.qml
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE
    )

    qt_add_qml_module(superterm-qml
        URI Superterm
        VERSION 1.0
        QML_FILES
            qml/Main.qml
            qml/TerminalPane.qml
            qml/SearchBar.qml
            qml/SettingsPanel.qml
            qml/ThumbnailSidebar.qml
            qml/Theme.qml
        RESOURCES
            icons/superterm.svg
    )

    target_link_libraries(superterm-qml PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
    )

    target_compile_definitions(superterm-qml PRIVATE
        SUPERTERM_QMLTERMWIDGET_IMPORT_DIR="${QMLTERMWIDGET_IMPORT_DIR}"
    )
endif()
