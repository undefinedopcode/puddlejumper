import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Superterm

/*
 * Left-hand tab sidebar with live terminal previews.
 *
 * The alternative to the top tab strip. Previews come from
 * TerminalPane::requestThumbnail(), which grabs the terminal item as painted,
 * so they show real content. Only visible tabs can be grabbed, so the owner
 * refreshes the current tab's thumbnail and keeps the last good image for the
 * rest.
 */
Rectangle {
    id: bar

    property var    tabModel: []
    property int    currentIndex: 0
    // Preview file path per tab index, plus a counter used to bust the Image
    // cache: the file contents change but the path does not.
    property var    previews: ({})
    property var    previewVersions: ({})
    property string colorScheme: ""

    signal tabActivated(int index)
    signal tabClosed(int index)
    signal addTabRequested()
    signal layoutToggleRequested()
    signal settingsRequested()

    width: 190
    color: Theme.chromeBg

    function setPreview(index, filePath) {
        // Reassign rather than mutate so bindings re-evaluate, and bump the
        // version so the Image reloads from the same path.
        const p = Object.assign({}, previews)
        p[index] = filePath
        previews = p
        const v = Object.assign({}, previewVersions)
        v[index] = (v[index] || 0) + 1
        previewVersions = v
    }

    // Previews are keyed by index, so closing a tab would otherwise shift every
    // later preview onto the wrong terminal.
    function shiftPreviews(fromIndex) {
        const p = {}
        const v = {}
        for (const key in previews) {
            const i = parseInt(key, 10)
            if (i < fromIndex) {
                p[i] = previews[key]
                v[i] = previewVersions[key]
            } else if (i > fromIndex) {
                p[i - 1] = previews[key]
                v[i - 1] = previewVersions[key]
            }
        }
        previews = p
        previewVersions = v
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- header ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 4

            Label {
                text: qsTr("Terminals")
                color: Theme.textDim
                font.pixelSize: 10
                font.letterSpacing: 0.8
                Layout.fillWidth: true
            }
            ToolButton {
                text: "＋"
                focusPolicy: Qt.NoFocus
                onClicked: bar.addTabRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("New tab (Ctrl+Shift+T)")

                contentItem: Text {
                    text: parent.text
                    color: Theme.textDim
                    font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: parent.hovered ? Theme.chromeBgAlt : "transparent"
                }
            }
            ToolButton {
                text: "▥"
                focusPolicy: Qt.NoFocus
                onClicked: bar.layoutToggleRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Use top tabs")

                contentItem: Text {
                    text: parent.text
                    color: Theme.textDim
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: parent.hovered ? Theme.chromeBgAlt : "transparent"
                }
            }
            ToolButton {
                text: "⚙"
                focusPolicy: Qt.NoFocus
                onClicked: bar.settingsRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings (Ctrl+,)")

                contentItem: Text {
                    text: parent.text
                    color: Theme.textDim
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 4
                    color: parent.hovered ? Theme.chromeBgAlt : "transparent"
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        // ---- thumbnail list ---------------------------------------------
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: bar.tabModel
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            topMargin: 8
            bottomMargin: 8

            delegate: Item {
                id: cell
                required property int index
                required property var modelData

                width: list.width
                height: card.height + spacing
                readonly property int spacing: 6
                readonly property bool isCurrent: bar.currentIndex === index

                Rectangle {
                    id: card
                    x: 8
                    width: parent.width - 16
                    height: preview.height + label.height + 14
                    radius: 6
                    color: cell.isCurrent ? Theme.chromeBgAlt : Theme.bg
                    border.width: cell.isCurrent ? 2 : 1
                    border.color: cell.isCurrent ? Theme.accent : Theme.border

                    // ---- preview ------------------------------------------
                    Rectangle {
                        id: preview
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 5
                        height: Math.round(width * 0.62)
                        radius: 3
                        clip: true
                        // Matches the terminal's own backdrop until a grab
                        // arrives, so cards do not flash the chrome colour.
                        color: Theme.terminalWell

                        Image {
                            id: thumb
                            anchors.fill: parent
                            source: bar.previews[cell.index]
                                    ? "file://" + bar.previews[cell.index] : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            visible: status === Image.Ready
                        }

                        // Shown until the first grab lands.
                        Text {
                            anchors.centerIn: parent
                            visible: thumb.status !== Image.Ready
                            text: cell.modelData.title !== ""
                                  ? cell.modelData.title : qsTr("Terminal")
                            color: Theme.textDim
                            font.pixelSize: 9
                            elide: Text.ElideRight
                            width: parent.width - 10
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    // ---- title row ----------------------------------------
                    RowLayout {
                        id: label
                        anchors.top: preview.bottom
                        anchors.topMargin: 4
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 5
                        anchors.rightMargin: 5
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            text: cell.modelData.title !== ""
                                  ? cell.modelData.title : qsTr("Terminal")
                            color: Theme.text
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        ToolButton {
                            visible: cell.isCurrent || cardHover.containsMouse
                            implicitWidth: 18
                            implicitHeight: 18
                            focusPolicy: Qt.NoFocus
                            text: "✕"
                            onClicked: bar.tabClosed(cell.index)

                            contentItem: Text {
                                text: parent.text
                                color: Theme.textDim
                                font.pixelSize: 9
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 3
                                color: parent.hovered ? Theme.border : "transparent"
                            }
                        }
                    }

                    MouseArea {
                        id: cardHover
                        anchors.fill: parent
                        hoverEnabled: true
                        // Behind the close button, which sits above it.
                        z: -1
                        onClicked: bar.tabActivated(cell.index)
                    }
                }
            }
        }
    }
}
