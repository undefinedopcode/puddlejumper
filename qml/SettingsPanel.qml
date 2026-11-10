import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Superterm

/*
 * Slide-in settings panel.
 *
 * The panel is a pure view: it reports edits via signals and never mutates
 * the properties it is bound to (that would break the caller's bindings).
 */
Rectangle {
    id: root

    property string colorScheme: "Solarized"
    property string fontFamily: ""
    property var    fontFamilies: []
    property real   fontSize: Theme.monoFontSize
    property real   opacityValue: 1.0
    property bool   blinkCursor: true
    property bool   boldIntense: true
    property var    schemes: []

    signal closeRequested()
    signal colorSchemeEdited(string value)
    signal fontFamilyEdited(string value)
    signal fontSizeEdited(real value)
    signal opacityEdited(real value)
    signal blinkCursorEdited(bool value)
    signal boldIntenseEdited(bool value)

    width: 280
    color: Theme.chromeBg
    border.color: Theme.border
    border.width: 1

    function close() { root.closeRequested() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Settings")
                color: Theme.text
                font.pixelSize: 15
                font.bold: true
                Layout.fillWidth: true
            }
            ToolButton {
                text: "\u2715"
                onClicked: root.close()
                contentItem: Text {
                    text: parent.text
                    color: Theme.textDim
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: parent.hovered ? Theme.chromeBgAlt : "transparent"
                }
            }
        }

        Label {
            text: qsTr("COLOUR SCHEME")
            color: Theme.textDim
            font.pixelSize: 10
            font.letterSpacing: 0.8
        }
        ComboBox {
            id: schemeBox
            Layout.fillWidth: true
            model: root.schemes
            currentIndex: Math.max(0, root.schemes.indexOf(root.colorScheme))
            font.pixelSize: Theme.uiFontSize
            onActivated: root.colorSchemeEdited(currentText)
        }

        Label {
            text: qsTr("FONT")
            color: Theme.textDim
            font.pixelSize: 10
            font.letterSpacing: 0.8
        }
        ComboBox {
            id: fontBox
            Layout.fillWidth: true
            model: root.fontFamilies
            currentIndex: Math.max(0, root.fontFamilies.indexOf(root.fontFamily))
            font.pixelSize: Theme.uiFontSize
            onActivated: root.fontFamilyEdited(currentText)
        }

        Label {
            text: qsTr("FONT SIZE")
            color: Theme.textDim
            font.pixelSize: 10
            font.letterSpacing: 0.8
        }
        RowLayout {
            Layout.fillWidth: true
            Slider {
                id: fontSlider
                Layout.fillWidth: true
                from: 8
                to: 28
                stepSize: 1
                value: root.fontSize
                onMoved: root.fontSizeEdited(value)
            }
            Label {
                text: Math.round(fontSlider.value) + "pt"
                color: Theme.text
                font.pixelSize: Theme.uiFontSize
                Layout.preferredWidth: 32
                horizontalAlignment: Text.AlignRight
            }
        }

        Label {
            text: qsTr("OPACITY")
            color: Theme.textDim
            font.pixelSize: 10
            font.letterSpacing: 0.8
        }
        RowLayout {
            Layout.fillWidth: true
            Slider {
                id: opacitySlider
                Layout.fillWidth: true
                from: 0.4
                to: 1.0
                stepSize: 0.05
                value: root.opacityValue
                onMoved: root.opacityEdited(value)
            }
            Label {
                text: Math.round(opacitySlider.value * 100) + "%"
                color: Theme.text
                font.pixelSize: Theme.uiFontSize
                Layout.preferredWidth: 40
                horizontalAlignment: Text.AlignRight
            }
        }

        Switch {
            text: qsTr("Blinking cursor")
            checked: root.blinkCursor
            font.pixelSize: Theme.uiFontSize
            onToggled: root.blinkCursorEdited(checked)
        }

        Switch {
            text: qsTr("Bold intense text")
            checked: root.boldIntense
            font.pixelSize: Theme.uiFontSize
            onToggled: root.boldIntenseEdited(checked)
        }

        Item { Layout.fillHeight: true }

        Label {
            text: qsTr("SHORTCUTS")
            color: Theme.textDim
            font.pixelSize: 10
            font.letterSpacing: 0.8
        }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 3
            Layout.fillWidth: true

            Repeater {
                model: [
                    ["Ctrl+Shift+T", "New tab"],
                    ["Ctrl+Shift+W", "Close tab"],
                    ["Ctrl+Tab", "Next tab"],
                    ["Ctrl+Shift+F", "Find"],
                    ["Ctrl+=", "Zoom in"],
                    ["Ctrl+-", "Zoom out"],
                    ["Ctrl+0", "Reset zoom"],
                    ["Ctrl+Shift+C", "Copy"],
                    ["Ctrl+Shift+V", "Paste"]
                ]
                Label {
                    required property var modelData
                    text: modelData[0]
                    color: Theme.accent
                    font.pixelSize: 10
                    font.family: "monospace"
                }
                Label {
                    required property var modelData
                    text: modelData[1]
                    color: Theme.textDim
                    font.pixelSize: 10
                }
            }
        }
    }
}
