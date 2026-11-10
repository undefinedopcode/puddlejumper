pragma Singleton

import QtQuick

/*
 * Palette and metrics for the window chrome.
 *
 * Under KDE the app selects the native org.kde.desktop QML style, which makes
 * SystemPalette report the user's actual Plasma widget colours. The chrome
 * then follows the desktop colour scheme — and keeps following it if the user
 * switches between light and dark while we are running.
 *
 * Elsewhere there is no meaningful system palette for a Qt Quick app, so we
 * fall back to a fixed dark palette that sits comfortably next to the common
 * terminal colour schemes.
 */
Item {
    id: theme

    visible: false
    width: 0
    height: 0

    // Live view of the system palette. Under the Fusion style this reports
    // Fusion's own defaults, but useSystemColors is false there, so the fixed
    // palette below is used instead.
    SystemPalette {
        id: sys
        colorGroup: SystemPalette.Active
    }

    // True when the chrome follows the desktop colour scheme.
    readonly property bool useSystemColors: chromeFollowsSystem

    readonly property color bg:          useSystemColors ? sys.window          : "#101216"
    readonly property color chromeBg:    useSystemColors ? sys.window          : "#171a20"
    readonly property color chromeBgAlt: useSystemColors ? sys.alternateBase   : "#1e2229"
    readonly property color border:      useSystemColors ? sys.mid             : "#2a2f38"
    readonly property color text:        useSystemColors ? sys.windowText      : "#d7dce5"
    readonly property color textDim:     useSystemColors ? sys.placeholderText : "#8a93a3"
    readonly property color accent:      useSystemColors ? sys.highlight       : "#4f9cf9"
    readonly property color accentText:  useSystemColors ? sys.highlightedText : "#ffffff"

    // Terminal background. Deliberately not the system window colour: the
    // terminal paints its own colour scheme, so the seam between chrome and
    // terminal is kept quiet rather than introducing a second competing hue.
    readonly property color terminalWell: useSystemColors ? sys.dark : "#101216"

    // Typography
    readonly property real  uiFontSize:   13
    readonly property real  monoFontSize: 13

    // Metrics
    readonly property int   radius:       8
    readonly property int   tabHeight:    34
    readonly property int   titleBarH:    38

    // Colour schemes shipped with QMLTermWidget.
    readonly property var colorSchemes: [
        "Solarized", "SolarizedLight", "Linux", "Ubuntu", "Nord",
        "DarkPastels", "Tango", "GreenOnBlack", "BlackOnWhite",
        "WhiteOnBlack", "BlackOnLightYellow", "BreezeModified",
        "BlackOnRandomLight", "Falcon"
    ]
}
