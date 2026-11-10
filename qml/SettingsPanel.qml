import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Superterm

/*
 * Centered modal settings window, sectioned down the left.
 *
 * This is a pure view: it reports edits via signals and never mutates the
 * properties it is bound to, which would sever the caller's bindings.
 */
Dialog {
    id: dialog

    // ---- values in ------------------------------------------------------
    property string colorScheme: "Solarized"
    property string fontFamily: ""
    property var    fontFamilies: []
    property real   fontSize: Theme.monoFontSize
    property real   opacityValue: 1.0
    property bool   blinkCursor: true
    property bool   boldIntense: true
    property bool   italicEnabled: true
    property bool   antialiasText: true
    property int    lineSpacing: 0
    property int    scrollback: 5000
    // The terminal already draws its own box-drawing characters; there is no
    // way to switch it from QML (inline C++ setter, no Q_PROPERTY). Shown
    // read-only so the behaviour is discoverable rather than mysterious.
    property bool   drawsOwnLineChars: true
    property var    schemes: []

    // ---- edits out ------------------------------------------------------
    signal colorSchemeEdited(string value)
    signal fontFamilyEdited(string value)
    signal fontSizeEdited(real value)
    signal opacityEdited(real value)
    signal blinkCursorEdited(bool value)
    signal boldIntenseEdited(bool value)
    signal italicEdited(bool value)
    signal antialiasEdited(bool value)
    signal lineSpacingEdited(int value)
    signal scrollbackEdited(int lineCount)

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(720, (Overlay.overlay ? Overlay.overlay.width : 720) - 80)
    height: Math.min(560, (Overlay.overlay ? Overlay.overlay.height : 560) - 80)
    padding: 0

    background: Rectangle {
        color: Theme.chromeBg
        radius: Theme.radius
        border.color: Theme.border
        border.width: 1
    }

    property int section: 0
    readonly property var sectionNames: [
        qsTr("Appearance"), qsTr("Text"), qsTr("Cursor"), qsTr("Scrolling")
    ]

    // Small helpers for the recurring row shapes.
    component SectionTitle: Label {
        color: Theme.text
        font.pixelSize: 13
        font.bold: true
    }

    component Hint: Label {
        color: Theme.textDim
        font.pixelSize: 11
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    contentItem: RowLayout {
        spacing: 0

        // ---- section list ------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 168
            Layout.fillHeight: true
            color: Theme.bg

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 2

                Repeater {
                    model: dialog.sectionNames

                    Rectangle {
                        id: sectionRow
                        required property int index
                        required property var modelData

                        Layout.fillWidth: true
                        height: 32
                        radius: 5
                        color: dialog.section === index ? Theme.accent
                             : sectionHover.containsMouse ? Theme.chromeBgAlt
                             : "transparent"

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: sectionRow.modelData
                            elide: Text.ElideRight
                            color: dialog.section === sectionRow.index
                                   ? Theme.accentText : Theme.text
                            font.pixelSize: Theme.uiFontSize
                        }

                        MouseArea {
                            id: sectionHover
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: dialog.section = sectionRow.index
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ---- page --------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 18
            spacing: 12

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: dialog.section

                // ============ Appearance ============
                ColumnLayout {
                    spacing: 10
                    SectionTitle { text: qsTr("Colour scheme") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: dialog.schemes
                        currentIndex: Math.max(0, dialog.schemes.indexOf(dialog.colorScheme))
                        font.pixelSize: Theme.uiFontSize
                        onActivated: dialog.colorSchemeEdited(currentText)
                    }

                    SectionTitle { text: qsTr("Opacity"); Layout.topMargin: 8 }
                    RowLayout {
                        Layout.fillWidth: true
                        Slider {
                            id: opacitySlider
                            Layout.fillWidth: true
                            from: 0.4
                            to: 1.0
                            stepSize: 0.05
                            value: dialog.opacityValue
                            onMoved: dialog.opacityEdited(value)
                        }
                        Label {
                            text: Math.round(opacitySlider.value * 100) + "%"
                            color: Theme.text
                            font.pixelSize: Theme.uiFontSize
                            Layout.preferredWidth: 44
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    Item { Layout.fillHeight: true }
                }

                // ============ Text ============
                ColumnLayout {
                    spacing: 10
                    SectionTitle { text: qsTr("Font") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: dialog.fontFamilies
                        currentIndex: Math.max(0, dialog.fontFamilies.indexOf(dialog.fontFamily))
                        font.pixelSize: Theme.uiFontSize
                        onActivated: dialog.fontFamilyEdited(currentText)
                    }
                    Hint {
                        text: qsTr("Only fixed-pitch families are listed, since the "
                                 + "terminal cell grid depends on them.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        spacing: 12

                        Label {
                            text: qsTr("Size")
                            color: Theme.text
                            font.pixelSize: Theme.uiFontSize
                        }
                        Slider {
                            id: sizeSlider
                            Layout.fillWidth: true
                            from: 8
                            to: 32
                            stepSize: 1
                            value: dialog.fontSize
                            onMoved: dialog.fontSizeEdited(value)
                        }
                        Label {
                            text: Math.round(sizeSlider.value) + "pt"
                            color: Theme.text
                            font.pixelSize: Theme.uiFontSize
                            Layout.preferredWidth: 40
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            text: qsTr("Line spacing")
                            color: Theme.text
                            font.pixelSize: Theme.uiFontSize
                        }
                        SpinBox {
                            from: -4
                            to: 12
                            value: dialog.lineSpacing
                            editable: true
                            onValueModified: dialog.lineSpacingEdited(value)
                        }
                        Label {
                            text: qsTr("px")
                            color: Theme.textDim
                            font.pixelSize: Theme.uiFontSize
                        }
                        Item { Layout.fillWidth: true }
                    }

                    SectionTitle { text: qsTr("Rendering"); Layout.topMargin: 8 }
                    CheckBox {
                        text: qsTr("Bold intense text")
                        checked: dialog.boldIntense
                        font.pixelSize: Theme.uiFontSize
                        onToggled: dialog.boldIntenseEdited(checked)
                    }
                    CheckBox {
                        text: qsTr("Italic")
                        checked: dialog.italicEnabled
                        font.pixelSize: Theme.uiFontSize
                        onToggled: dialog.italicEdited(checked)
                    }
                    CheckBox {
                        text: qsTr("Antialias text")
                        checked: dialog.antialiasText
                        font.pixelSize: Theme.uiFontSize
                        onToggled: dialog.antialiasEdited(checked)
                    }

                    Item { Layout.fillHeight: true }
                }

                // ============ Cursor ============
                ColumnLayout {
                    spacing: 10
                    CheckBox {
                        text: qsTr("Blinking cursor")
                        checked: dialog.blinkCursor
                        font.pixelSize: Theme.uiFontSize
                        onToggled: dialog.blinkCursorEdited(checked)
                    }
                    Hint {
                        text: qsTr("The cursor only blinks while the terminal holds "
                                 + "keyboard focus.")
                    }
                    Item { Layout.fillHeight: true }
                }

                // ============ Scrolling ============
                ColumnLayout {
                    spacing: 10
                    SectionTitle { text: qsTr("Scrollback") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            text: qsTr("Lines kept")
                            color: Theme.text
                            font.pixelSize: Theme.uiFontSize
                        }
                        SpinBox {
                            from: 0
                            to: 100000
                            stepSize: 500
                            editable: true
                            value: dialog.scrollback
                            onValueModified: dialog.scrollbackEdited(value)
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Hint {
                        text: qsTr("Scrollback depth for new tabs.")
                    }

                    SectionTitle { text: qsTr("Line drawing"); Layout.topMargin: 8 }
                    CheckBox {
                        text: qsTr("Terminal draws its own box characters")
                        checked: dialog.drawsOwnLineChars
                        enabled: false
                        font.pixelSize: Theme.uiFontSize
                    }
                    Hint {
                        text: qsTr("Always on. Box frames and block art are painted as "
                                 + "vectors rather than taken from the font, so they stay "
                                 + "crisp and grid-aligned at any point size. "
                                 + "QMLTermWidget enables this unconditionally and does "
                                 + "not expose a switch.")
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---- footer --------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("Changes apply immediately and are remembered.")
                    color: Theme.textDim
                    font.pixelSize: 11
                    Layout.fillWidth: true
                }
                Button {
                    text: qsTr("Close")
                    focusPolicy: Qt.NoFocus
                    onClicked: dialog.close()
                }
            }
        }
    }
}
