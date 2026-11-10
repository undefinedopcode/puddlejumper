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
    property string workingDirectory: ""
    property string shellProgram: ""
    property var    shellProgramArgs: []

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

    QMLTermWidget {
        id: term
        anchors.fill: parent

        font.family: pane.monoFont !== "" ? pane.monoFont : defaultMonoFont
        font.pointSize: pane.fontSize
        colorScheme: pane.colorScheme

        enableBold: pane.boldIntense
        fullCursorHeight: false
        blinkingCursor: pane.blinking
        antialiasText: true

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
