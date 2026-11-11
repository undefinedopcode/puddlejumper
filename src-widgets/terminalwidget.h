/*
 * superterm — Qt Widgets port.
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
    TerminalWidget(int id, const Settings &settings,
                   const TerminalProfile &profile, QWidget *parent = nullptr);

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

    /* A live preview of the terminal as painted. Empty if unavailable. */
    QPixmap previewPixmap(const QSize &size) const;

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

private:
    void onOutput();
    void showContextMenu(const QPoint &globalPos);

    int m_id = 0;
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

    bool m_canCopy = false;
};
