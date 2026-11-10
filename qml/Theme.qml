pragma Singleton
import QtQuick

QtObject {
    // Palette — tuned to sit comfortably next to common dark terminal schemes.
    readonly property color bg:           "#101216"
    readonly property color chromeBg:     "#171a20"
    readonly property color chromeBgAlt:  "#1e2229"
    readonly property color border:       "#2a2f38"
    readonly property color text:         "#d7dce5"
    readonly property color textDim:      "#8a93a3"
    readonly property color accent:       "#4f9cf9"
    readonly property color accentSoft:   "#1f3a5f"

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
