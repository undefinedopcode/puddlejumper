import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PuddleJumper

/*
 * Inline search bar overlay for a terminal pane.
 */
Rectangle {
    id: root

    property var    target: null
    property bool   matchFound: false
    signal closed()

    height: 40
    radius: Theme.radius
    color: Theme.chromeBgAlt
    border.color: Theme.border
    border.width: 1

    function open() {
        visible = true
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    function close() {
        visible = false
        if (root.target && root.target.focusTerminal)
            root.target.focusTerminal()
        root.closed()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        spacing: 6

        TextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: qsTr("Find in terminal…")
            font.pixelSize: Theme.uiFontSize
            color: Theme.text
            background: Rectangle {
                color: Theme.bg
                radius: 4
                border.color: searchField.activeFocus ? Theme.accent : Theme.border
                border.width: 1
            }
            onTextChanged: {
                if (root.target && text.length > 0)
                    root.target.searchFor(text)
            }
            onAccepted: {
                if (root.target && text.length > 0)
                    root.target.searchFor(text)
            }
            Keys.onEscapePressed: root.close()
        }

        ToolButton {
            text: qsTr("↑")
            font.pixelSize: Theme.uiFontSize
            onClicked: {
                if (root.target && searchField.text.length > 0)
                    root.target.searchFor(searchField.text)
            }
        }

        ToolButton {
            text: qsTr("✕")
            font.pixelSize: Theme.uiFontSize
            onClicked: root.close()
        }
    }
}
