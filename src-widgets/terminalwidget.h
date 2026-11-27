/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

#include <functional>
#include <memory>

#include "profiles.h"
#include "programresume.h"
#include "resumerules.h"
#include "settings.h"


class ProgramStatus;
class QLabel;
class QTermWidget;
class QTimer;
class QMenu;
class QPoint;

namespace Konsole {
class TerminalDisplay;
}

/*
 * One terminal tab.
 *
 * Bundles a QTermWidget with the bits the widgets API does not provide:
 * a stable id (for preview files), the live title, the ability to re-apply
 * the whole appearance in one call, and a right-click context menu that
 * reuses QTermWidget's built-in copy/paste, zoom and search actions.
 *
 * Each tab is created from a profile. The profile's overrides are resolved
 * against the global Settings every time applyAppearance() runs, so a global
 * font or theme change still reaches fields the profile leaves alone, while
 * an overridden field stays put.
 */
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    /*
     * Where the terminal's input and output go. A Local terminal runs a shell
     * in a pty. An External one has no process: its output arrives through
     * feed() and its keystrokes leave through externalInput(). tmux control
     * mode panes are External.
     */
    enum class Backend { Local, External };

    /*
     * The frame round a pane in a split tab: none when a tab has one pane,
     * otherwise a thin border in the highlight colour for the active pane and
     * in the background colour for the rest.
     */
    enum class PaneFrame { None, Active, Inactive };

    TerminalWidget(int id, const Settings &settings,
                   const TerminalProfile &profile, QWidget *parent = nullptr,
                   Backend backend = Backend::Local);

    int id() const { return m_id; }

    QTermWidget *terminal() const { return m_terminal; }
    /* Local files dropped on the terminal go to `handler` rather than being
     * typed as their paths; it returns false to have them typed after all.
     * An empty handler puts things back. */
    void setFileDropHandler(std::function<bool(const QStringList &paths)> handler)
    {
        m_fileDrop = std::move(handler);
    }

    QString sessionTitle() const { return m_title; }

    /* The profile this tab was opened with (by value: the store may mutate). */
    const TerminalProfile &profile() const { return m_profile; }
    QString profileId() const { return m_profile.id; }
    QString profileName() const { return m_profile.name; }

    /*
     * The colour scheme actually in force after profile resolution.
     *
     * QTermWidget offers no getter, so this is tracked. The status bar needs
     * it: showing the *global* scheme there would be wrong for a tab whose
     * profile overrides it.
     */
    QString appliedColorScheme() const { return m_appliedColorScheme; }

    /* A colour scheme for this pane alone (a Rex action set it), over its
     * profile's and the global one; empty for none. Kept with the pane. */
    QString colorSchemeOverride() const { return m_schemeOverride; }
    void setColorSchemeOverride(const QString &scheme);

    /* Font size actually in force, after profile resolution. */
    double appliedFontSize() const;

    /*
     * Re-apply the profile onto a changed profile definition. Tabs keep their
     * shell, but theme/font changes take effect immediately.
     */
    void setProfile(const TerminalProfile &profile);

    /* Re-apply every appearance setting. Safe to call on live terminals. */
    void applyAppearance();

    /* Re-apply scrollback depth / scrollbar / flow-control. */
    void applyScrolling();

    /* Re-apply activity/silence monitoring. */
    void applyMonitoring();

    void sendText(const QString &text);
    void focusTerminal();

    bool isExternal() const { return m_backend == Backend::External; }

    /* External terminals: output to display, and the screen size to keep. */
    void feed(const QByteArray &data);
    void setScreenSize(int columns, int lines);
    /* External terminals: a note in the pane's top corner while its size
     * comes from elsewhere (another client); empty takes it away. */
    void setFollowing(const QString &note);
    bool isFollowing() const;

    /* One character cell, in pixels. */
    QSize cellSize() const;
    /*
     * Pixels this widget adds round its character grid: frame, margins and
     * scrollbar. Width W pixels hold (W - chromeSize().width()) / cell width
     * columns.
     */
    QSize chromeSize() const;

    void setPaneFrame(PaneFrame frame);
    PaneFrame paneFrame() const { return m_frame; }
    /* Width of the frame on each side of a framed pane. */
    static int paneBorder();
    /*
     * Pane cards (experimental, Settings: appearance/paneCards): a framed
     * tiled pane is drawn as floating panes are, a rounded panel with a
     * soft edge and a small title strip, at low contrast. Panes in floating
     * layers keep the plain frame (their float has the title).
     */
    static void setPaneCards(bool on);
    static bool paneCards();
    /* The gap between split panes and round framed ones (appearance). */
    static int paneGap();
    /* What a card's title strip says: for a local pane the foreground
     * program and its directory ("micro  ·  ~/src"), else the title. */
    QString cardTitle() const;
    /* A card's title until the program sets one (a Rex block's label). */
    void setFallbackTitle(const QString &title);
    /* Not a tiled pane: in a floating layer, so never a card. */
    void setFloating(bool floating);
    /* The pixels a framed pane's frame takes, across and down: a card's
     * (tiled) or the plain border's. */
    static QSize frameChrome(bool tiled = true);
    /* All a tmux or Rex pane has round its cells, across and down: its frame
     * and the terminal margin on each side (no scrollbar). Such a pane is
     * sized to cells * cell size plus this. */
    static QSize externalChrome(bool tiled = true);
    /*
     * Quiet focus (Settings: appearance/quietFocus): the focused pane's
     * frame (a card's, a float's, the plain border) a brighter edge like
     * the others', not the accent. frameEdgeColour() is a frame's edge.
     */
    static void setQuietFocus(bool on);
    static bool quietFocus();
    static QColor frameEdgeColour(const QColor &bg, const QColor &fg, bool active, bool floating);
    /* The colour scheme's background. */
    QColor themeBackground() const;
    /* The colour of a card's title strip as it's drawn now. */
    QColor cardStripColour() const;
    /* A card's title strip: its height, and where it is in the pane. */
    static int cardTitleHeight();
    bool isCardShown() const { return isCard(); }

    /* The shell's current directory, for opening a split there. Local only. */
    QString currentDirectory() const;

    /*
     * Foreground process state, for the quit warning.
     *
     * hasRunningProcess() is true when the pty's foreground process group is
     * not the shell's own group -- i.e. a program (an editor, a build, ssh)
     * currently owns the terminal and would be killed if we quit. It is false
     * at an idle prompt, so a window full of shells never triggers a warning.
     *
     * The comparison is made against getpgid(shellPid), not shellPid itself:
     * the two coincide for a normal login shell (it is its own group leader)
     * but need not in general.
     */
    bool hasRunningProcess() const;
    /* Name of the foreground program ("" when it cannot be determined). */
    QString foregroundProgramName() const;
    /* The foreground program's arguments (from /proc), when one is running. */
    QStringList foregroundCommandLine() const;

    /* The screen's size in pixels at the current font: columns x lines cells. */
    QSize screenPixelSize() const;
    /*
     * A picture of the screen, as large as fits in `size` (logical pixels)
     * keeping its shape. Works while the pane is hidden. Empty if unavailable.
     */
    QPixmap previewPixmap(const QSize &size, qreal devicePixelRatio = 1.0) const;

    /*
     * Screenshots. Both report through screenshotTaken().
     *
     * takeScreenshot() captures the visible screen: the character grid plus
     * its margin, without the scrollbar. startRegionScreenshot() lets the user
     * drag out a rectangle first (see ScreenshotOverlay); cancelling it
     * reports nothing.
     */
    void takeScreenshot();
    /* What takeScreenshot() reports, returned; null without a display. */
    QPixmap screenshot() const;
    void startRegionScreenshot();

    /*
     * Content tracking.
     *
     * receivedData() fires from the pty, so it fires for hidden tabs too.
     * That makes it usable as a dirty flag: the sidebar only needs to
     * re-capture a tab whose output has changed since the last grab, instead
     * of rotating blindly through every tab.
     */
    bool hasNewOutput() const { return m_dirty; }
    void clearNewOutput() { m_dirty = false; }

    /* Monotonic count of output chunks, for cheap change detection. */
    quint64 outputSeq() const { return m_outputSeq; }

    /*
     * What the programs here report of themselves (OSC 7501, see
     * ProgramStatus). Working, blocked and idle records go once the
     * foreground program that reported them is no longer in the foreground
     * (it exited, or the shell is back at its prompt); done and error ones
     * count as seen once a key is pressed here.
     */
    ProgramStatus *programStatus() const { return m_status; }
    /* The program that reported has gone (said by a tmux pane's session,
     * or a shell's prompt): its working, blocked and idle records go. */
    void programEnded();
    /* External panes: whether their source answers the program status
     * query itself (tmux 3.9 does), so that it isn't answered twice. */
    void setSourceAnswersProgramStatus(bool answers);
    /* The source decodes programs' clipboard writes itself (a Rex server, with
     * clipboard_written events): the emulator leaves them, and doesn't answer. */
    void setSourceHandlesClipboard(bool handles);
    /* A program here wrote the clipboard by the source's say-so: as if the
     * emulator had (clipboardWritten). */
    void reportClipboardWrite(bool primary, qint64 characters, const QStringList &types)
    {
        emit clipboardWritten(primary, characters, types);
    }

    /*
     * Program resume (OSC 7577, see ProgramResume). A local pane gives its
     * shell a token (OSC7577_TOKEN) and keeps what the programs here report.
     * resumeVerdict() is what the rules in force (resumerules.h) make of the
     * newest record, and the directory it would resume in; not trusted, with
     * no reason, when there is none.
     */
    resumerules::Verdict resumeVerdict(QString *cwd = nullptr);
    /*
     * After a restore: start `command` in `cwd` once the shell is ready for
     * input (its first prompt, or quiet), by typing the helper
     * ("puddlejumper --resume-helper FILE"), which runs it from a file only
     * the user can read, never as shell source.
     */
    void resumeProgram(const QStringList &command, const QString &cwd);
    /* Where those files go; the restore clears what's left there. */
    static QString resumeDirectory();

signals:
    void titleChanged(const QString &title);
    void sessionFinished();
    /* Coalesced so a burst of output does not spam the window. */
    void outputArrived();
    /* Ask the owner to adjust everyone's font size (the shared Settings). */
    void zoomInRequested();
    void zoomOutRequested();
    void zoomResetRequested();
    /*
     * Monitoring signals, fed from QTermWidget::activity() / silence() when
     * the corresponding scrolling/monitor* setting is on. The owner decides
     * how to surface them (tab marker, bell, &c).
     */
    void activityDetected();
    void silenceDetected();

    void screenshotTaken(const QPixmap &image);

    /* External terminals: bytes typed (or mouse reports) to deliver. */
    void externalInput(const QByteArray &data);
    /* A mouse button was pressed on the screen. */
    void pointerPressed();

    /* From the context menu; the owner decides what a split means. */
    void splitRequested(Qt::Orientation orientation);
    void closePaneRequested();

    /* The pane changed size (and so, soon, the terminal's columns and lines),
     * or an external pane was given new columns and lines. */
    void resized();

    /* A program here reported its status (programStatus() has it). */
    void programStatusReported();

    /* A program here put something on the clipboard (or the primary
     * selection): the text's length, -1 if none; its types. */
    void clipboardWritten(bool primary, qint64 characters, const QStringList &types);

    /* A link in the output was Ctrl-clicked, or opened from the menu. */
    void linkOpened(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /* QTermWidget's display widget, which it doesn't expose. */
    Konsole::TerminalDisplay *display() const;

    void onOutput();
    void onProgramStatusReport(const QString &body);
    void checkProgramStatusOwner();
    void watchProgramStatusOwner();
    /* Who is in the foreground, as far as status records care. */
    int statusOwner() const;
    void showContextMenu(const QPoint &globalPos);
    /* The web address under a point on screen; none if no link is there. */
    QUrl linkAt(const QPoint &globalPos) const;

    int m_id = 0;
    Backend m_backend = Backend::Local;
    PaneFrame m_frame = PaneFrame::None;
    const Settings &m_settings;
    TerminalProfile m_profile;
    QString m_schemeOverride;         // setColorSchemeOverride()
    QTermWidget *m_terminal = nullptr;
    std::function<bool(const QStringList &)> m_fileDrop;
    QLabel *m_followBadge = nullptr;  // setFollowing()
    bool m_floating = false;          // setFloating()
    QString m_fallbackTitle;          // setFallbackTitle()
    // A local card's title: the foreground program and where it is.
    QString m_cardProgram;
    QString m_cardDir;
    QTimer *m_cardWatch = nullptr;
    void refreshCardContext();
    bool isCard() const;
    void applyFrameMargins();
    void placeFollowBadge();
    QString m_title;

    /*
     * The colour scheme actually in force, tracked because QTermWidget has no
     * getter for it. Needed so a profile that overrides the scheme can be
     * told apart from one that inherits it after a global change.
     */
    QString m_appliedColorScheme;

    std::unique_ptr<ProgramResume> m_resume;   // local panes
    ProgramStatus *m_status = nullptr;
    // The foreground process group when a record was last reported, and a
    // look every so often while one is held that its going ends.
    int m_statusOwner = 0;
    QTimer *m_statusWatch = nullptr;
    bool m_statusFromShell = false;   // reported while the shell had the foreground
    bool m_statusSawCommand = false;  // ... and a command has run since
    bool m_promptMarks = false;       // the shell marks its prompts (OSC 133)

    bool m_dirty = false;
    quint64 m_outputSeq = 0;
    quint64 m_reportedSeq = 0;
};
