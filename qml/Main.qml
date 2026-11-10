import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QMLTermWidget 2.0
import Superterm

ApplicationWindow {
    id: win

    width: 1000
    height: 640
    minimumWidth: 480
    minimumHeight: 320
    visible: true
    color: Theme.bg
    title: activeSessionTitle !== "" ? activeSessionTitle + " — SuperTerm" : "SuperTerm"
    flags: Qt.Window

    property alias colorScheme: themeState.colorScheme
    property alias monoFont: themeState.monoFont
    property alias monoFontSize: themeState.monoFontSize
    property alias termOpacity: themeState.termOpacity
    property alias blinking: themeState.blinking
    property alias boldIntense: themeState.boldIntense

    property string activeSessionTitle: ""
    property string activeDirectory: ""
    property bool   settingsOpen: false

    // Central state shared by every tab.
    QtObject {
        id: themeState
        property string colorScheme: "Solarized"
        property string monoFont: defaultMonoFont
        property real   monoFontSize: Theme.monoFontSize
        property real   termOpacity: 1.0
        property bool   blinking: true
        property bool   boldIntense: true
    }

    component TabButton: Rectangle {
        id: tb
        required property int    tabIndex
        required property string label
        property bool isCurrent: tabs.currentIndex === tabIndex

        height: Theme.tabHeight
        width: Math.min(220, Math.max(120, labelText.implicitWidth + 58))
        radius: 6
        color: isCurrent ? Theme.chromeBgAlt
             : hover.containsMouse ? Theme.chromeBg
             : "transparent"
        border.color: isCurrent ? Theme.border : "transparent"
        border.width: 1

        Text {
            id: labelText
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 44
            text: tb.label
            color: tb.isCurrent ? Theme.text : Theme.textDim
            font.pixelSize: Theme.uiFontSize
            elide: Text.ElideRight
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.MiddleButton)
                    win.closeTab(tb.tabIndex)
                else
                    tabs.currentIndex = tb.tabIndex
            }
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            width: 24
            height: 24
            visible: hover.containsMouse || tb.isCurrent
            text: "✕"
            font.pixelSize: 11
            onClicked: win.closeTab(tb.tabIndex)

            background: Rectangle {
                radius: 4
                color: parent.hovered ? Theme.border : "transparent"
            }
            contentItem: Text {
                text: parent.text
                color: Theme.textDim
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    // ---- Tab model -----------------------------------------------------
    // Each entry: { title, cwd }. Panes live in the StackLayout by index.
    property var tabModel: []

    function newTab() {
        // Always reassign: in-place push() emits no change signal for var
        // arrays, so the Repeater/ListView would never see the new entry.
        const t = tabModel.slice()
        t.push({ title: qsTr("Terminal"), cwd: "" })
        tabModel = t
        tabs.currentIndex = t.length - 1
        Qt.callLater(win.focusCurrentTab)
        return tabs.currentIndex
    }

    function closeTab(index) {
        if (index < 0 || index >= tabModel.length)
            return
        const t = tabModel.slice()
        t.splice(index, 1)
        tabModel = t
        if (tabModel.length === 0)
            newTab()
        else
            tabs.currentIndex = Math.min(index, tabModel.length - 1)
    }

    function activePane() {
        return paneRepeater.itemAt(tabs.currentIndex)
    }

    // Keystrokes go to whichever item holds active focus. Switching tabs
    // removes the previous item from the focus chain, so focus must be handed
    // to the newly-current terminal explicitly, after the layout has settled.
    function focusCurrentTab() {
        const p = activePane()
        if (p && p.focusTerminal)
            p.focusTerminal()
    }

    Component.onCompleted: {
        newTab()
        Qt.callLater(function() {
            const p = activePane()
            if (p && p.focusTerminal) p.focusTerminal()
        })
    }


    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Tab strip --------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            height: Theme.titleBarH
            color: Theme.chromeBg

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4

                ListView {
                    id: tabs
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    orientation: ListView.Horizontal
                    spacing: 4
                    clip: true
                    model: win.tabModel
                    currentIndex: 0
                    onCurrentIndexChanged: Qt.callLater(win.focusCurrentTab)

                    delegate: TabButton {
                        required property int index
                        required property var modelData
                        tabIndex: index
                        label: modelData.title
                    }
                }

                ToolButton {
                    text: "＋"
                    font.pixelSize: 16
                    onClicked: win.newTab()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("New tab (Ctrl+Shift+T)")

                    contentItem: Text {
                        text: parent.text
                        color: Theme.textDim
                        font.pixelSize: 16
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
                    font.pixelSize: 14
                    onClicked: win.settingsOpen = !win.settingsOpen

                    contentItem: Text {
                        text: parent.text
                        color: win.settingsOpen ? Theme.accent : Theme.textDim
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
        }

        // ---- Terminal area ----------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            StackLayout {
                id: paneStack
                anchors.fill: parent
                currentIndex: tabs.currentIndex

                Repeater {
                    id: paneRepeater
                    model: win.tabModel

                    TerminalPane {
                        id: tabPane
                        monoFont: win.monoFont
                        colorScheme: win.colorScheme
                        fontSize: win.monoFontSize
                        blinking: win.blinking
                        boldIntense: win.boldIntense
                        opacity: win.termOpacity
                        workingDirectory: modelData.cwd !== undefined ? modelData.cwd : ""
                        shellProgram: envShell
                        shellProgramArgs: envShellArgs
                        // Captured explicitly: `model`/`modelData` are not
                        // reliably in scope inside signal handlers.
                        property int tabIndex: index

                        onTitleChanged: {
                            if (tabPane.tabIndex === tabs.currentIndex)
                                win.activeSessionTitle = title
                        }
                        onCurrentDirChanged: {
                            if (tabPane.tabIndex === tabs.currentIndex)
                                win.activeDirectory = currentDir
                        }
                        onSessionFinished: win.closeTab(tabPane.tabIndex)
                    }
                }
            }

            // Search overlay
            SearchBar {
                id: searchBar
                visible: false
                target: win.activePane()
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                width: Math.min(420, parent.width - 32)
            }
        }

        // ---- Status bar --------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            height: 24
            color: Theme.chromeBg

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 12

                Label {
                    text: win.activeDirectory !== "" ? win.activeDirectory : "~"
                    color: Theme.textDim
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
                Label {
                    text: win.colorScheme
                    color: Theme.textDim
                    font.pixelSize: 11
                }
                Label {
                    text: Math.round(win.monoFontSize) + "pt"
                    color: Theme.textDim
                    font.pixelSize: 11
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: (wheel) => {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        win.monoFontSize = Math.max(8, Math.min(28,
                            win.monoFontSize + (wheel.angleDelta.y > 0 ? 1 : -1)))
                    }
                }
            }
        }
    }

    // ---- Settings drawer ------------------------------------------------
    SettingsPanel {
        id: settings
        visible: win.settingsOpen
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.titleBarH
        anchors.bottomMargin: 24
        x: win.settingsOpen ? win.width - width : win.width
        opacity: win.settingsOpen ? 1 : 0

        colorScheme: win.colorScheme
        fontFamily: win.monoFont
        fontFamilies: monoFontFamilies
        fontSize: win.monoFontSize
        opacityValue: win.termOpacity
        blinkCursor: win.blinking
        boldIntense: win.boldIntense
        schemes: Theme.colorSchemes

        onColorSchemeEdited: (value) => win.colorScheme = value
        onFontFamilyEdited: (value) => win.monoFont = value
        onFontSizeEdited: (value) => win.monoFontSize = value
        onOpacityEdited: (value) => win.termOpacity = value
        onBlinkCursorEdited: (value) => win.blinking = value
        onBoldIntenseEdited: (value) => win.boldIntense = value
        onCloseRequested: win.settingsOpen = false

        Behavior on x {
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
    }

    // ---- Global shortcuts -----------------------------------------------
    Shortcut {
        sequence: "Ctrl+Shift+T"
        onActivated: win.newTab()
    }
    Shortcut {
        sequence: "Ctrl+Shift+W"
        onActivated: win.closeTab(tabs.currentIndex)
    }
    Shortcut {
        sequence: "Ctrl+Tab"
        onActivated: tabs.currentIndex = (tabs.currentIndex + 1) % Math.max(1, win.tabModel.length)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        onActivated: tabs.currentIndex = (tabs.currentIndex - 1 + win.tabModel.length) % Math.max(1, win.tabModel.length)
    }
    Shortcut {
        sequence: "Ctrl+Shift+F"
        onActivated: searchBar.open()
    }
    Shortcut {
        sequence: "Ctrl+="
        onActivated: win.monoFontSize = Math.min(28, win.monoFontSize + 1)
    }
    Shortcut {
        sequence: "Ctrl+Minus"
        onActivated: win.monoFontSize = Math.max(8, win.monoFontSize - 1)
    }
    Shortcut {
        sequence: "Ctrl+0"
        onActivated: win.monoFontSize = Theme.monoFontSize
    }
    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: { const p = win.activePane(); if (p && p.copySelection) p.copySelection() }
    }
    Shortcut {
        sequence: "Ctrl+Shift+V"
        onActivated: { const p = win.activePane(); if (p && p.pasteClipboard) p.pasteClipboard() }
    }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (searchBar.visible) searchBar.close()
            else if (win.settingsOpen) win.settingsOpen = false
        }
    }
}
