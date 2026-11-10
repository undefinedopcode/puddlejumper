cmake_minimum_required(VERSION 3.21)

project(superterm
    VERSION 0.1.0
    DESCRIPTION "A modern terminal built on Qt Quick and QMLTermWidget"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Widgets)
qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Locate the QMLTermWidget QML module.
#
# QMLTermWidget is a QML plugin (import QMLTermWidget 2.0), NOT a linkable
# library. Locate its module directory at configure time; main.cpp puts it on
# the engine's import path.
#
# IMPORTANT: do NOT link against qtermwidget6 (the Qt Widgets library) as well.
# The plugin vendors its own copy of the Konsole/KPty sources, so linking the
# Widgets library pulls in a second copy of those symbols. The app then
# renders blank terminal content and aborts on exit ("corrupted
# double-linked list"). The plugin .so is self-contained.
# ---------------------------------------------------------------------------
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
        "QMLTermWidget QML module not found. Install it first, e.g.:\n"
        "  Arch:   pacman -S qmltermwidget\n"
        "  Debian: apt install qmltermwidget-qt6-dev\n"
        "or build https://github.com/Swordfish90/qmltermwidget and point\n"
        "-DQMLTERMWIDGET_IMPORT_DIR=<prefix>/lib/qt6/qml at it.")
endif()
message(STATUS "QMLTermWidget import dir: ${QMLTERMWIDGET_IMPORT_DIR}")

qt_add_executable(superterm
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

qt_add_qml_module(superterm
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

target_link_libraries(superterm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
)

target_compile_definitions(superterm PRIVATE
    SUPERTERM_QMLTERMWIDGET_IMPORT_DIR="${QMLTERMWIDGET_IMPORT_DIR}"
)

install(TARGETS superterm
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Desktop entry and icon, so the app appears in launchers and the xdg portal
# recognises its app id (which also silences the startup portal warning).
include(GNUInstallDirs)
install(FILES data/superterm.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES icons/superterm.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

qt_generate_deploy_qml_app_script(
    TARGET superterm
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# ---------------------------------------------------------------------------
# Widgets spike (branch explore/qtwidgets only)
#
# A separate executable so it can never affect the QML app. This one DOES link
# qtermwidget6, which the QML build must not do (see the note above): here the
# Widgets library is the entire point, and the QML plugin is not loaded, so the
# duplicate-symbol problem does not arise.
#
#   cmake --build build --target superterm-widgets
#   ./build/superterm-widgets
# ---------------------------------------------------------------------------
option(SUPERTERM_BUILD_WIDGETS_SPIKE "Build the Qt Widgets port spike" ON)

if(SUPERTERM_BUILD_WIDGETS_SPIKE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(QTERMWIDGET6 REQUIRED IMPORTED_TARGET qtermwidget6)

    qt_add_executable(superterm-widgets
        spike-widgets/main.cpp
        spike-widgets/mainwindow.cpp
        spike-widgets/mainwindow.h
    )
    target_link_libraries(superterm-widgets PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        PkgConfig::QTERMWIDGET6
    )
    set_target_properties(superterm-widgets PROPERTIES
        AUTOMOC ON
        OUTPUT_NAME superterm-widgets
    )
    # The QML target disables AUTOMOC at project level; this one needs it for
    # MainWindow's Q_OBJECT.
endif()
