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
    property alias italicEnabled: themeState.italicEnabled
    property alias antialiasText: themeState.antialiasText
    property alias lineSpacing: themeState.lineSpacing
    property alias scrollback: themeState.scrollback
    // "standard" = top tab strip, "thumbnails" = left preview sidebar.
    property alias tabLayout: themeState.tabLayout

    property string activeSessionTitle: ""
    property string activeDirectory: ""

    // ---- Persistence ---------------------------------------------------
    // Values are written straight through on change: settings are small and
    // a burst of slider drags is harmless, whereas losing them on a crash is
    // not.
    readonly property string kColorScheme: "appearance/colorScheme"
    readonly property string kFont:        "appearance/fontFamily"
    readonly property string kFontSize:    "appearance/fontSize"
    readonly property string kOpacity:     "appearance/opacity"
    readonly property string kBlink:       "appearance/blinkingCursor"
    readonly property string kBold:        "appearance/boldIntense"
    readonly property string kItalic:      "appearance/italic"
    readonly property string kAntialias:   "appearance/antialias"
    readonly property string kLineSpacing: "appearance/lineSpacing"
    readonly property string kScrollback:  "scrolling/scrollback"
    readonly property string kTabLayout:   "appearance/tabLayout"

    function loadSettings() {
        // Guard: context properties are absent when running under a bare
        // qml runtime, in which case we simply keep the defaults.
        if (typeof settingsStore === "undefined" || settingsStore === null)
            return

        const scheme = settingsStore.stringValue(kColorScheme, win.colorScheme)
        if (Theme.colorSchemes.indexOf(scheme) !== -1)
            win.colorScheme = scheme

        const font = settingsStore.stringValue(kFont, win.monoFont)
        if (font !== "" && (typeof monoFontFamilies === "undefined"
                            || monoFontFamilies.indexOf(font) !== -1))
            win.monoFont = font

        win.monoFontSize = quantizeFontSize(
            settingsStore.doubleValue(kFontSize, win.monoFontSize))
        win.termOpacity = settingsStore.doubleValue(kOpacity, win.termOpacity)
        win.blinking = settingsStore.boolValue(kBlink, win.blinking)
        win.boldIntense = settingsStore.boolValue(kBold, win.boldIntense)
        win.italicEnabled = settingsStore.boolValue(kItalic, win.italicEnabled)
        win.antialiasText = settingsStore.boolValue(kAntialias, win.antialiasText)
        win.lineSpacing = settingsStore.intValue(kLineSpacing, win.lineSpacing)
        win.scrollback = settingsStore.intValue(kScrollback, win.scrollback)

        const layout = settingsStore.stringValue(kTabLayout, win.tabLayout)
        if (layout === "standard" || layout === "thumbnails")
            win.tabLayout = layout

        restoreGeometry()
    }

    // Font size is offered in half-point steps. Snap programmatically-set
    // values too: Slider.SnapAlways only constrains user interaction, so a
    // value restored from config (or set via a shortcut) could otherwise be
    // 13.7 while the UI displays 13.5.
    function quantizeFontSize(pt) {
        return Math.round(pt * 2) / 2
    }

    function formatPt(pt) {
        const v = quantizeFontSize(pt)
        return (v % 1 === 0 ? v.toFixed(0) : v.toFixed(1)) + "pt"
    }

    function saveSetting(key, value) {
        if (typeof settingsStore === "undefined" || settingsStore === null)
            return
        if (typeof value === "number")
            settingsStore.setDouble(key, value)
        else if (typeof value === "boolean")
            settingsStore.setBool(key, value)
        else
            settingsStore.setString(key, value)
    }

    function restoreGeometry() {
        if (typeof settingsStore === "undefined" || settingsStore === null)
            return
        if (!settingsStore.contains("window/width"))
            return
        const w = settingsStore.intValue("window/width", win.width)
        const h = settingsStore.intValue("window/height", win.height)
        if (w > 0 && h > 0) {
            win.width = Math.max(win.minimumWidth, w)
            win.height = Math.max(win.minimumHeight, h)
        }
        const x = settingsStore.intValue("window/x", -1)
        const y = settingsStore.intValue("window/y", -1)
        if (x >= 0 && y >= 0) {
            // Window.x / Window.y are writable properties, not setters.
            win.x = x
            win.y = y
        }
        if (settingsStore.boolValue("window/maximized", false))
            win.showMaximized()
    }

    function saveGeometry() {
        if (typeof settingsStore === "undefined" || settingsStore === null)
            return
        // Store the normal (un-maximised) geometry so un-maximising restores
        // a sensible size.
        const maximized = win.visibility === Window.Maximized
        settingsStore.saveWindow(Math.round(win.x), Math.round(win.y),
                                 Math.round(win.width), Math.round(win.height),
                                 maximized)
    }

    // Central state shared by every tab.
    QtObject {
        id: themeState
        property string colorScheme: "Solarized"
        property string monoFont: defaultMonoFont
        property real   monoFontSize: Theme.monoFontSize
        property real   termOpacity: 1.0
        property bool   blinking: true
        property bool   boldIntense: true
        property bool   italicEnabled: true
        property bool   antialiasText: true
        property int    lineSpacing: 0
        property int    scrollback: 5000
        property string tabLayout: "standard"
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
            // Chrome must never take focus from the terminal; pressing it
            // would otherwise leave keystrokes bound to the button.
            focusPolicy: Qt.NoFocus
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
    // Each entry: { title, cwd, id }. Panes live in the StackLayout by index.
    property var tabModel: []
    property int nextTabId: 1

    function newTab() {
        // Always reassign: in-place push() emits no change signal for var
        // arrays, so the Repeater/ListView would never see the new entry.
        const t = tabModel.slice()
        t.push({ title: qsTr("Terminal"), cwd: "", id: win.nextTabId++ })
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
        // Keep previews aligned with the shifted indices.
        sidebar.shiftPreviews(index)
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

    // Thumbnails can only be grabbed from a visible terminal, so only the
    // current tab is refreshed; the sidebar keeps the last good image for the
    // others. Refreshing on tab change keeps previews reasonably current
    // without grabbing a hidden item.
    // Capture the current pane's preview. Grabbing a hidden item yields an
    // empty image, so only the visible tab can be sampled; every other card
    // keeps whatever was captured the last time it was on screen.
    function refreshThumbnail() {
        if (tabLayout !== "thumbnails")
            return
        const p = activePane()
        if (p && p.requestThumbnail)
            p.requestThumbnail()
    }

    onTabLayoutChanged: {
        win.saveSetting(win.kTabLayout, tabLayout)
        Qt.callLater(win.refreshThumbnail)
    }

    Component.onCompleted: {
        loadSettings()
        newTab()
        Qt.callLater(function() {
            const p = activePane()
            if (p && p.focusTerminal) p.focusTerminal()
            win.refreshThumbnail()
        })
    }

    // Refresh the current tab's preview periodically so the sidebar tracks
    // live output. Deliberately slow: each grab renders the terminal, and the
    // preview is a thumbnail, not a mirror.
    Timer {
        interval: 3000
        repeat: true
        running: win.tabLayout === "thumbnails"
        onTriggered: win.refreshThumbnail()
    }

    // Save geometry on the way out; the appearance settings are already
    // persisted as they change.
    onClosing: saveGeometry()


    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sidebar or top tabs, then the terminal. Wrapping the two halves in a
        // RowLayout lets the sidebar sit to the left without disturbing the
        // status bar below.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Thumbnail sidebar (only in "thumbnails" layout).
            ThumbnailSidebar {
                id: sidebar
                visible: win.tabLayout === "thumbnails"
                Layout.fillHeight: true
                tabModel: win.tabModel
                currentIndex: tabs.currentIndex
                colorScheme: win.colorScheme

                onTabActivated: (index) => tabs.currentIndex = index
                onTabClosed: (index) => win.closeTab(index)
                onAddTabRequested: win.newTab()
                onLayoutToggleRequested: win.tabLayout = "standard"
                onSettingsRequested: settings.open()
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

        // ---- Tab strip --------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            height: Theme.titleBarH
            visible: win.tabLayout === "standard"
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
                    onCurrentIndexChanged: {
                        Qt.callLater(win.focusCurrentTab)
                        Qt.callLater(win.refreshThumbnail)
                    }

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
                    focusPolicy: Qt.NoFocus
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
                    focusPolicy: Qt.NoFocus
                    text: win.tabLayout === "thumbnails" ? "▤" : "▥"
                    font.pixelSize: 14
                    onClicked: win.tabLayout = win.tabLayout === "thumbnails"
                                             ? "standard" : "thumbnails"
                    ToolTip.visible: hovered
                    ToolTip.text: win.tabLayout === "thumbnails"
                        ? qsTr("Use top tabs") : qsTr("Use thumbnail sidebar")

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
                    focusPolicy: Qt.NoFocus
                    text: "⚙"
                    font.pixelSize: 14
                    onClicked: settings.open()

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
        }

        // ---- Terminal area ----------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Backstop behind the terminal: visible only for the instant before
            // the terminal paints, or if a pane has no session.
            Rectangle {
                anchors.fill: parent
                color: Theme.terminalWell
            }

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
                        italicEnabled: win.italicEnabled
                        antialiasText: win.antialiasText
                        lineSpacing: win.lineSpacing
                        scrollback: win.scrollback
                        opacity: win.termOpacity
                        workingDirectory: modelData.cwd !== undefined ? modelData.cwd : ""
                        shellProgram: envShell
                        shellProgramArgs: envShellArgs
                        // Captured explicitly: `model`/`modelData` are not
                        // reliably in scope inside signal handlers.
                        property int tabIndex: index
                        // Stable id for the preview file, so it survives
                        // reordering when a tab is closed.
                        property int tabIdSeed: modelData.id !== undefined
                                               ? modelData.id : index
                        tabId: tabPane.tabIdSeed

                        onTitleChanged: {
                            if (tabPane.tabIndex === tabs.currentIndex)
                                win.activeSessionTitle = title
                        }
                        onCurrentDirChanged: {
                            if (tabPane.tabIndex === tabs.currentIndex)
                                win.activeDirectory = currentDir
                        }
                        onSessionFinished: win.closeTab(tabPane.tabIndex)
                        onThumbnailReady: (filePath, version) =>
                            sidebar.setPreview(tabPane.tabIndex, filePath)
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
            }   // inner ColumnLayout
        }       // RowLayout

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
                    text: win.formatPt(win.monoFontSize)
                    color: Theme.textDim
                    font.pixelSize: 11
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: (wheel) => {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        // Ctrl+wheel zooms in half-point steps, matching the
                        // slider and the keyboard shortcuts.
                        win.monoFontSize = Math.max(8, Math.min(32,
                            win.monoFontSize + (wheel.angleDelta.y > 0 ? 0.5 : -0.5)))
                    }
                }
            }
        }
    }

    // ---- Settings ------------------------------------------------------
    SettingsPanel {
        id: settings

        colorScheme: win.colorScheme
        fontFamily: win.monoFont
        fontFamilies: monoFontFamilies
        fontSize: win.monoFontSize
        opacityValue: win.termOpacity
        blinkCursor: win.blinking
        boldIntense: win.boldIntense
        italicEnabled: win.italicEnabled
        antialiasText: win.antialiasText
        lineSpacing: win.lineSpacing
        scrollback: win.scrollback
        schemes: Theme.colorSchemes
        tabLayout: win.tabLayout

        onTabLayoutEdited: (layout) => win.tabLayout = layout

        onColorSchemeEdited: (value) => {
            win.colorScheme = value
            win.saveSetting(win.kColorScheme, value)
        }
        onFontFamilyEdited: (value) => {
            win.monoFont = value
            win.saveSetting(win.kFont, value)
        }
        onFontSizeEdited: (value) => {
            const pt = win.quantizeFontSize(value)
            win.monoFontSize = pt
            win.saveSetting(win.kFontSize, pt)
        }
        onOpacityEdited: (value) => {
            win.termOpacity = value
            win.saveSetting(win.kOpacity, value)
        }
        onBlinkCursorEdited: (value) => {
            win.blinking = value
            win.saveSetting(win.kBlink, value)
        }
        onBoldIntenseEdited: (value) => {
            win.boldIntense = value
            win.saveSetting(win.kBold, value)
        }
        onItalicEdited: (value) => {
            win.italicEnabled = value
            win.saveSetting(win.kItalic, value)
        }
        onAntialiasEdited: (value) => {
            win.antialiasText = value
            win.saveSetting(win.kAntialias, value)
        }
        onLineSpacingEdited: (value) => {
            win.lineSpacing = value
            win.saveSetting(win.kLineSpacing, value)
        }
        onScrollbackEdited: (value) => {
            win.scrollback = value
            win.saveSetting(win.kScrollback, value)
        }

        // Closing hands focus back to the terminal.
        onClosed: Qt.callLater(win.focusCurrentTab)
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
        onActivated: win.monoFontSize = Math.min(32, win.monoFontSize + 0.5)
    }
    Shortcut {
        sequence: "Ctrl+Minus"
        onActivated: win.monoFontSize = Math.max(8, win.monoFontSize - 0.5)
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
        sequence: "Ctrl+Comma"
        onActivated: settings.open()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            // The Dialog closes itself on Escape; only the search bar needs
            // handling here.
            if (searchBar.visible)
                searchBar.close()
        }
    }
}
