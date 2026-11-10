import QtQuick
import QMLTermWidget 2.0
import Superterm

/*
 * TerminalPane wraps a single QMLTermWidget + QMLTermSession pair.
 *
 * Type names: qmltermwidget registers `QMLTermWidget` (the display; C++ class
 * Konsole::TerminalDisplay) and `QMLTermSession` (the session; C++ class
 * KSession). There is no `TerminalDisplay`/`TerminalSession` QML type.
 */
Item {
    id: pane

    property string colorScheme: "Solarized"
    property string monoFont: ""
    property real   fontSize: Theme.monoFontSize
    property bool   blinking: true
    property bool   boldIntense: true
    property bool   italicEnabled: true
    property bool   antialiasText: true
    property int    lineSpacing: 0
    property int    scrollback: 5000
    property string workingDirectory: ""
    property string shellProgram: ""
    property var    shellProgramArgs: []
    // Stable per-tab identifier, used to name the preview file.
    property int    tabId: 0

    // Reported back to the tab bar / window title.
    property string title: ""
    property string currentDir: ""
    property bool   hasActiveProcess: false

    signal sessionFinished()

    function copySelection() { term.copyClipboard() }
    function pasteClipboard() { term.pasteClipboard() }
    function sendText(t) { session.sendText(t) }
    function focusTerminal() { term.forceActiveFocus() }

    function searchFor(text) {
        if (text.length > 0)
            session.search(text)
    }

    /*
     * Live preview for the thumbnail sidebar.
     *
     * grabToImage() returns a QQuickItemGrabResult whose .url is an internal
     * "itemgrabber:#N" identifier, not something an Image can load. Passing
     * the result object itself to Image.source works, but the result is
     * destroyed once we drop it, so the caller keeps a reference for as long
     * as the preview is displayed.
     *
     * Only the visible tab can usefully be grabbed; a hidden item has nothing
     * painted to sample.
     */
    /*
     * Live preview for the thumbnail sidebar.
     *
     * grabToImage() samples the terminal exactly as painted, so previews show
     * real content. Two constraints shape this:
     *   - The result's .url is an internal "itemgrabber:#N" identifier that
     *     Image cannot load, and assigning the result object to Image.source
     *     silently does nothing. It has to be written to a file.
     *   - Results must stay referenced until written, so the write happens
     *     inside the callback.
     *
     * Files are named per tab and reused, so we do not accumulate a file per
     * refresh. Only the visible tab can usefully be grabbed.
     */
    property bool   thumbnailPending: false
    property int    thumbnailVersion: 0
    signal thumbnailReady(string filePath, int version)

    function requestThumbnail() {
        if (thumbnailPending || !term.grabToImage || term.width <= 0)
            return
        thumbnailPending = true
        const v = thumbnailVersion + 1
        const target = thumbnailStore.thumbnailPathFor(pane.tabId, v)
        term.grabToImage(function(result) {
            thumbnailPending = false
            if (!result || !result.saveToFile(target))
                return
            pane.thumbnailVersion = v
            pane.thumbnailReady(target, v)
        }, Qt.size(Math.max(1, Math.round(term.width * 0.22)),
                  Math.max(1, Math.round(term.height * 0.22))))
    }

    QMLTermWidget {
        id: term
        anchors.fill: parent

        font.family: pane.monoFont !== "" ? pane.monoFont : defaultMonoFont
        font.pointSize: pane.fontSize
        colorScheme: pane.colorScheme

        enableBold: pane.boldIntense
        enableItalic: pane.italicEnabled
        fullCursorHeight: false
        blinkingCursor: pane.blinking
        antialiasText: pane.antialiasText
        lineSpacing: pane.lineSpacing

        session: QMLTermSession {
            id: session
            onTitleChanged: pane.title = title
            onFinished: pane.sessionFinished()
        }

        // Start the shell only once the display has a real size. Starting
        // earlier (e.g. from Component.onCompleted) races the layout and can
        // leave the pane painting a background with no glyphs.
        property bool started: false

        function tryStart() {
            if (started || width <= 0 || height <= 0)
                return
            started = true
            startTimer.start()
        }

        onWidthChanged: tryStart()
        onHeightChanged: tryStart()
        Component.onCompleted: tryStart()

        Timer {
            id: startTimer
            interval: 0
            repeat: false
            onTriggered: {
                if (pane.workingDirectory !== "")
                    session.initialWorkingDirectory = pane.workingDirectory
                if (pane.shellProgram !== "")
                    session.shellProgram = pane.shellProgram
                if (pane.shellProgramArgs.length > 0)
                    session.shellProgramArgs = pane.shellProgramArgs
                session.startShellProgram()
            }
        }
    }

    // currentDir and hasActiveProcess are READ-only with no NOTIFY signal,
    // so poll them.
    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            pane.currentDir = session.currentDir
            pane.hasActiveProcess = session.hasActiveProcess
        }
    }
}
