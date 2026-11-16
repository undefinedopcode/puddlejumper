# Vendored QTermWidget (lxqt/qtermwidget 2.4.0, commit 4ca192e5), built as a
# static library for superterm. See SUPERTERM.md for what differs from
# upstream and why.
#
# This replaces upstream's CMakeLists.txt, which needs lxqt2-build-tools and
# installs a shared library, headers and data files. Here nothing is installed:
# keyboard layouts and colour schemes are compiled in as Qt resources, so the
# application has no runtime dependency on a system qtermwidget package.

set(QTERMWIDGET_VERSION_MAJOR 2)
set(QTERMWIDGET_VERSION_MINOR 4)
set(QTERMWIDGET_VERSION_PATCH 0)

add_library(qtermwidget STATIC
    lib/BlockArray.cpp
    lib/ColorScheme.cpp
    lib/Emulation.cpp
    lib/Filter.cpp
    lib/History.cpp
    lib/HistorySearch.cpp
    lib/KeyboardTranslator.cpp
    lib/konsole_wcwidth.cpp
    lib/qtermwidget.cpp
    lib/Screen.cpp
    lib/ScreenWindow.cpp
    lib/SearchBar.cpp
    lib/SearchBar.ui
    lib/Session.cpp
    lib/ShellCommand.cpp
    lib/TerminalCharacterDecoder.cpp
    lib/TerminalDisplay.cpp
    lib/tools.cpp
    lib/Vt102Emulation.cpp
)

# The terminal process: in a pty, or on Windows in a pseudoconsole (ConPTY).
if(WIN32)
    target_sources(qtermwidget PRIVATE lib/Pty_win.cpp)
    # The pseudoconsole API is declared from Windows 10 1809 on.
    target_compile_definitions(qtermwidget PRIVATE
        _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
else()
    target_sources(qtermwidget PRIVATE
        lib/kprocess.cpp
        lib/kpty.cpp
        lib/kptydevice.cpp
        lib/kptyprocess.cpp
        lib/Pty.cpp
    )
endif()

set_target_properties(qtermwidget PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(qtermwidget PUBLIC Qt6::Widgets)

include(GenerateExportHeader)
generate_export_header(qtermwidget
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/lib/qtermwidget_export.h"
    BASE_NAME QTERMWIDGET
)
configure_file(lib/qtermwidget_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/lib/qtermwidget_version.h" @ONLY
)

# Consumers include <qtermwidget.h>, <Emulation.h> etc. directly, as they did
# against the system package's include directory.
target_include_directories(qtermwidget PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    "${CMAKE_CURRENT_BINARY_DIR}/lib"
)

# The export header expands QTERMWIDGET_EXPORT to visibility attributes unless
# this is defined, which is wrong for a static library.
target_compile_definitions(qtermwidget PUBLIC QTERMWIDGET_STATIC_DEFINE)

include(CheckFunctionExists)
check_function_exists(updwtmpx HAVE_UPDWTMPX)
if(HAVE_UPDWTMPX)
    target_compile_definitions(qtermwidget PRIVATE HAVE_UPDWTMPX)
endif()

# macOS has only the utmpx API; the legacy utmp functions kpty.cpp falls back
# to (setutent, getutline, ...) don't exist there.
if(APPLE)
    target_compile_definitions(qtermwidget PRIVATE HAVE_UTMPX)
endif()

# Data lives in Qt resources (see below). There are no compiled translations:
# upstream's are only for the search bar, which superterm doesn't use, so
# TRANSLATIONS_DIR points at an empty resource path and loading quietly finds
# nothing.
target_compile_definitions(qtermwidget PRIVATE
    "KB_LAYOUT_DIR=\":/qtermwidget/kb-layouts\""
    "COLORSCHEMES_DIR=\":/qtermwidget/color-schemes\""
    "TRANSLATIONS_DIR=\":/qtermwidget/translations\""
    HAVE_POSIX_OPENPT
    HAVE_SYS_TIME_H
)

file(GLOB QTERMWIDGET_KEYTABS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/kb-layouts/*.keytab")
file(GLOB QTERMWIDGET_SCHEMES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/color-schemes/*.colorscheme")

qt_add_resources(qtermwidget "qtermwidget_data"
    PREFIX "/qtermwidget"
    BASE lib
    FILES ${QTERMWIDGET_KEYTABS} ${QTERMWIDGET_SCHEMES}
)
