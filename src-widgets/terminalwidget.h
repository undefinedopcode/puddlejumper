/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

#include "profiles.h"
#include "settings.h"

class QTermWidget;
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

    /* From the context menu; the owner decides what a split means. */
    void splitRequested(Qt::Orientation orientation);
    void closePaneRequested();

    /* The pane changed size (and so, soon, the terminal's columns and lines). */
    void resized();

    /* A link in the output was Ctrl-clicked, or opened from the menu. */
    void linkOpened(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /* QTermWidget's display widget, which it doesn't expose. */
    Konsole::TerminalDisplay *display() const;

    void onOutput();
    void showContextMenu(const QPoint &globalPos);
    /* The web address under a point on screen; none if no link is there. */
    QUrl linkAt(const QPoint &globalPos) const;

    int m_id = 0;
    Backend m_backend = Backend::Local;
    PaneFrame m_frame = PaneFrame::None;
    const Settings &m_settings;
    TerminalProfile m_profile;
    QTermWidget *m_terminal = nullptr;
    QString m_title;

    /*
     * The colour scheme actually in force, tracked because QTermWidget has no
     * getter for it. Needed so a profile that overrides the scheme can be
     * told apart from one that inherits it after a global change.
     */
    QString m_appliedColorScheme;

    bool m_dirty = false;
    quint64 m_outputSeq = 0;
    quint64 m_reportedSeq = 0;
};
