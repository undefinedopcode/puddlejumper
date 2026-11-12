/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QSettings>

/*
 * Application settings, backed by QSettings.
 *
 * Deliberately a plain C++ class rather than the QML-facing wrapper in src/:
 * widgets do not need Q_INVOKABLE marshalling, so this reads and writes typed
 * values directly.
 *
 * Key layout is kept identical to the QML build so an existing config keeps
 * working after the port.
 */
class Settings : public QObject
{
    Q_OBJECT

public:
    struct Appearance {
        QString colorScheme = QStringLiteral("Solarized");
        QString fontFamily;                 // resolved at startup if empty
        double  fontSize = 13.0;            // 0.5pt steps
        bool    blinkingCursor = true;
        bool    boldIntense = true;
        bool    italic = true;
        bool    antialias = true;
        /*
         * Draw box-drawing and block characters as vectors rather than taking
         * them from the font. Defaults to true because that is the library
         * default too; exposed as a setting because a font with good
         * box-drawing glyphs may be preferable.
         */
        bool    drawLineChars = true;
        int     lineSpacing = 0;                // extra pixels between rows
        QString tabLayout = QStringLiteral("standard"); // |thumbnails

        /*
         * Terminal background opacity, 0.3..1.0. Revives the QML build's
         * appearance/opacity key, which the widgets API can serve after all
         * (setTerminalOpacity). Clamped so the text can never vanish entirely.
         */
        double  opacity = 1.0;

        /*
         * Cursor glyph: 0 = block, 1 = underline, 2 = I-beam. Stored as an int
         * rather than the Konsole enum so Settings stays free of terminal
         * headers; TerminalWidget maps it onto KeyboardCursorShape.
         */
        int     cursorShape = 0;

        /*
         * Empty space painted around the terminal grid, in pixels. Konsole's
         * "margin"; handy to let the terminal breathe inside its window.
         */
        int     terminalMargin = 4;

        /*
         * Enable bidirectional text rendering (Konsole's "bidi"). Needed for
         * correctly mixed RTL/LTR text; harmless to pure-LTR sessions.
         */
        bool    bidi = true;
    };

    struct Scrolling {
        int scrollback = 5000;

        /*
         * Scrollbar position: none | left | right (QTermWidget::ScrollBar-
         * Position). Right matches the long-standing default for this codebase;
         * left is convenient in the thumbnail layout. Stored as a string so the
         * config file stays human-readable.
         */
        QString scrollbarPosition = QStringLiteral("right");

        /*
         * Warn once when flow control (XON/XOFF) stops the display updating;
         * otherwise a Ctrl-S "freeze" looks like a hung terminal.
         */
        bool    flowControlWarning = true;

        /*
         * Watch a terminal for output arriving (activity) or stopping
         * (silence) and surface it on the tab. Off by default -- the kind of
         * noise that helps for long builds hurts in an interactive shell.
         */
        bool    monitorActivity = false;

        /*
         * Report when a terminal produces no output for silenceTimeout
         * seconds. Handy as a "build finished" cue.
         */
        bool    monitorSilence = false;
        int     silenceTimeout = 10;    // seconds

        /*
         * Before closing the window or quitting, ask for confirmation if any
         * terminal is running a foreground program -- an editor, a build, an
         * ssh session -- since quitting kills it. Off only for users who find
         * the prompt intrusive; a prompt is the safe default.
         */
        bool    confirmCloseWithRunningProcesses = true;

        /*
         * Paste handling (QTermWidget's own options). Both off by default,
         * which is what the library does. These are input behaviour rather
         * than scrolling, but live here because they are applied with the rest
         * of this struct by TerminalWidget::applyScrolling(); their keys are
         * under input/.
         */
        bool    confirmMultilinePaste = false;
        bool    trimPastedTrailingNewlines = false;
    };

    explicit Settings(QObject *parent = nullptr);

    const Appearance &appearance() const { return m_appearance; }
    const Scrolling  &scrolling() const { return m_scrolling; }

    void setColorScheme(const QString &v);
    void setFontFamily(const QString &v);
    void setFontSize(double v);
    void setBlinkingCursor(bool v);
    void setBoldIntense(bool v);
    void setItalic(bool v);
    void setAntialias(bool v);
    void setDrawLineChars(bool v);
    void setLineSpacing(int v);
    void setTabLayout(const QString &v);
    void setOpacity(double v);
    void setCursorShape(int v);
    void setTerminalMargin(int v);
    void setBidi(bool v);
    void setScrollback(int v);
    void setScrollbarPosition(const QString &v);
    void setFlowControlWarning(bool v);
    void setConfirmCloseWithRunningProcesses(bool v);
    void setMonitorActivity(bool v);
    void setMonitorSilence(bool v);
    void setSilenceTimeout(int v);
    void setConfirmMultilinePaste(bool v);
    void setTrimPastedTrailingNewlines(bool v);

    /*
     * Point the running session at a throwaway copy of a scheme, while keeping
     * `realScheme` as the persisted choice.
     *
     * qtermwidget caches schemes by basename and cannot reload the same name,
     * so an in-place edit has to be applied through a freshly-named copy. That
     * copy is valid only for this process, so the config keeps the real name
     * and finaliseLiveSchemes() puts it back before the next write.
     */
    void setLiveScheme(const QString &realScheme, const QString &livePath);

    /* Is this path the live stand-in currently in force? */
    bool isLiveScheme(const QString &path) const
    {
        return !m_liveSchemePath.isEmpty() && path == m_liveSchemePath;
    }

    /* The real scheme name the live stand-in is standing in for. */
    QString liveSchemeRealName() const { return m_liveSchemeRealName; }

    /*
     * Swap the in-memory colour scheme back to the real name and drop the live
     * mapping. A no-op when nothing was overridden.
     */
    void finaliseLiveSchemes();

    /* Delete live copies belonging to this session, leaving the real schemes. */
    static void removeLiveSchemeFiles();

    void saveWindowGeometry(const QByteArray &geometry, const QByteArray &state);
    QByteArray windowGeometry() const;
    QByteArray windowState() const;
    /* The thumbnail sidebar's width in pixels; 0 when never set. */
    int sidebarWidth() const;
    void setSidebarWidth(int pixels);

signals:
    void appearanceChanged();
    void scrollingChanged();
    /*
     * Emitted when any monitor* setting changes. Kept separate from
     * scrollingChanged because it drives setMonitor*() calls, not history
     * sizing.
     */
    void monitoringChanged();

private:
    void load();

    QSettings m_settings;
    Appearance m_appearance;
    Scrolling m_scrolling;

    /*
     * Live scheme override for this process, set by setLiveScheme(): the
     * throwaway file the terminals are actually using, and the real scheme it
     * stands in for. Empty livePath means "no override".
     */
    QString m_liveSchemePath;
    QString m_liveSchemeRealName;
};
