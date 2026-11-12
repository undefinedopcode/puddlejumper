/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "profiles.h"
#include "terminaltab.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QStringList>

class QTimer;
class Settings;
class TerminalWidget;
class TmuxControl;

/*
 * One tmux session shown natively: each tmux window is a superterm tab, each
 * pane a superterm pane, laid out exactly as tmux lays them out.
 *
 * tmux owns the truth. Panes hold no process; their output arrives through
 * control mode and their keystrokes go back with send-keys. User actions
 * (new tab, split, close, resize, switching tab or pane) become tmux commands,
 * and the tabs change when tmux's notifications say they have. So whatever is
 * done here is there next time, from superterm or any other tmux client, and
 * detaching or quitting leaves every program running.
 */
class TmuxSession : public QObject
{
    Q_OBJECT

public:
    /*
     * label:   how menus and tooltips name this session ("main",
     *          "host: main").
     * command: a control-mode client to run, e.g.
     *          {"tmux", "-u", "-C", "new-session", "-A", "-s", "main"}, or
     *          the same behind ssh.
     */
    TmuxSession(const QString &label, const QStringList &command,
                const Settings &settings, const TerminalProfile &profile,
                QObject *parent = nullptr);
    ~TmuxSession() override;

    void start();
    /* Disconnect, leaving the session running. ended() follows. */
    void detach();
    /* Whether ended() came from detach() rather than the session going. */
    bool detachRequested() const { return m_detachRequested; }

    QString label() const { return m_label; }
    QString sessionName() const { return m_sessionName; }

    bool ownsTab(const TerminalTab *tab) const;
    bool ownsPane(const TerminalWidget *pane) const;
    QList<TerminalTab *> tabs() const;
    QString windowName(const TerminalTab *tab) const;

    // User actions, carried out by tmux.
    void newWindow();
    void splitPane(TerminalWidget *pane, Qt::Orientation orientation);
    void killPane(TerminalWidget *pane);
    void killWindow(TerminalTab *tab);
    void renameWindow(TerminalTab *tab, const QString &name);
    void selectWindow(TerminalTab *tab);

    /* The pixels a tab's layout has; becomes the client size in cells. */
    void setClientArea(const QSize &pixels);

signals:
    /* A window appeared; the receiver shows `tab`. */
    void tabAdded(TerminalTab *tab, const QString &name);
    void tabRenamed(TerminalTab *tab, const QString &name);
    /* tmux made this the current window. */
    void tabActivated(TerminalTab *tab);
    /* A window went; the receiver takes the tab away and deletes it. */
    void tabRemoved(TerminalTab *tab);
    /* The client has gone (detached, session killed, connection lost). All
     * tabs have been removed already. `reason` may be empty. */
    void ended(const QString &reason);

private:
    struct Window {
        QPointer<TerminalTab> tab;
        QString name;
        PaneLayout layout;
    };

    void onReady();
    void restoreWindows();
    void addWindow(int id, const QByteArray &layout, const QString &name, bool active);
    void removeWindow(int id);
    void applyLayout(int windowId);
    void relayoutAll();
    TerminalWidget *paneFor(int paneId);
    TerminalWidget *makePane(int paneId);
    void restorePane(int paneId, bool alternate, int cursorX, int cursorY);
    void resyncPane(int paneId);
    void sendInput(int paneId, const QByteArray &bytes);
    void sendClientSize();

    int paneId(const TerminalWidget *pane) const;
    int windowId(const TerminalTab *tab) const;
    QSize cellSize();
    QSize paneChrome() const;

    QString m_label;
    QStringList m_command;
    const Settings &m_settings;
    TerminalProfile m_profile;
    TmuxControl *m_control = nullptr;
    QString m_sessionName;

    QHash<int, Window> m_windows;
    QHash<int, QPointer<TerminalWidget>> m_panes;
    // Output for panes not yet created (their window's layout is still on the
    // way), and panes whose output is ignored until their history is restored.
    QHash<int, QByteArray> m_earlyOutput;
    QSet<int> m_restoring;
    bool m_attaching = true;   // until the initial windows are restored
    int m_activeWindow = -1;   // tmux's current window, as last reported

    // An invisible pane that only measures the cell size for the current font.
    TerminalWidget *m_metrics = nullptr;

    QSize m_clientArea;
    QSize m_clientCells;
    QTimer *m_sizeTimer = nullptr;

    // A splitter drag, sent once the drag pauses.
    QTimer *m_resizeTimer = nullptr;
    int m_resizePane = -1;
    Qt::Orientation m_resizeOrientation = Qt::Horizontal;
    int m_resizeCells = 0;

    bool m_ended = false;
    bool m_detachRequested = false;
};
