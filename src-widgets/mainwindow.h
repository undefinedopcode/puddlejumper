/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPixmap>
#include <QVector>

#include "profiles.h"
#include "settings.h"
#include "workspace.h"
#include "workspacefile.h"

class QTabWidget;
class QSplitter;
class QKeyEvent;
class QLabel;
class QMenu;
class QTimer;
class QToolButton;
class Notifier;
class Tab;
class TerminalTab;
struct ProgramStatusRecord;
class TerminalWidget;
class TmuxSession;
class SettingsDialog;
class TabStrip;
class ThumbnailSidebar;
class VncTab;
class WebTab;
class QUrl;

/*
 * Main window: tabs (or thumbnail sidebar) around a set of terminals.
 *
 * Mirrors the QML build's feature set, with the additions the widgets API
 * makes possible: working scrollback, line-drawing toggle, cursor shape, and a
 * built-in search bar.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    /*
     * Open what the command line named: a folder (its project workspace
     * file, or a workspace for the folder), a workspace file, or the name of
     * one in the workspace directories.
     */
    void openWorkspaceTarget(const QString &target);
    /* A tab showing the screen at `address` (host[:port|:display]) over VNC. */
    void openVnc(const QString &address);
    /* The same over RDP: host or host:port. */
    void openRdp(const QString &address);
    /* A tab showing a web page. */
    void openWeb(const QUrl &url);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /* Open a tab using the default profile. */
    void newTab();
    /* Open a tab using a specific profile id; falls back to the default. */
    void newTabWithProfile(const QString &profileId);
    void newWslTab(const QString &distribution);
    void closeTab(int index);
    void applyAppearanceToAll();
    void updateStatus();
    void updateTabLayout();

private:
    /* The active pane of the current tab: what window actions act on. */
    TerminalWidget *currentTerminal() const;
    /* The current tab or one by index, whatever it shows... */
    Tab *currentPage() const;
    Tab *pageAt(int index) const;
    /* ...or only if it shows terminals. */
    TerminalTab *currentTab() const;
    TerminalTab *tabAt(int index) const;
    /* The tab holding `pane`, and its index (-1 if none). */
    TerminalTab *tabOf(const QWidget *pane) const;
    int tabIndexOf(const QWidget *pane) const;
    /* Every pane in every tab, in every workspace. */
    QList<TerminalWidget *> allPanes() const;

    /*
     * Workspaces. The tab widget holds the shown workspace's tabs; the
     * others' are parked (hidden, still running) in their Workspace.
     */
    QList<Tab *> allPages() const;
    /* Those of them showing terminals. */
    QList<TerminalTab *> allTabs() const;
    /* The workspace a tab is in, as an index; -1 if none. */
    int workspaceOf(const Tab *tab) const;
    int workspaceIndex(const QString &id) const;
    void switchWorkspace(int index);
    /* Bring a tab on screen, switching workspace if need be. */
    void showTab(Tab *tab);
    /* Ask for a new workspace's details; its index, or -1 if cancelled.
     * `show` switches to it, which opens its first tab. */
    int newWorkspace(bool show);
    void editWorkspace(int index);
    void closeWorkspace(int index);
    /* Move a tab (a tmux tab: its whole session) to another workspace. */
    void moveTabToWorkspace(Tab *tab, int index);
    void rebuildWorkspaceMenu();
    /* Workspace name and colour on the toolbar, sidebar and menu. */
    void updateWorkspaceChrome();
    /* Mark a tab as having output that hasn't been seen. */
    void markTabBusy(Tab *tab);
    void onCurrentTabChanged(int index);

    /*
     * Workspace files (smug's format). Opening one asks for any variables it
     * needs, asks before running the commands of a file that isn't the
     * user's own, runs before_start, then opens its windows as a new
     * workspace's tabs. One already open is switched to instead.
     */
    void openWorkspaceFile(const QString &path);
    /* A folder: its .puddlejumper.yml or .smug.yml if it has one, else a plain
     * workspace for it (or the one already open there). */
    void openFolder(const QString &dir);
    /* Offer, in the status bar, a workspace file for where the shell is. */
    void updateProjectNotice();
    /* Read a workspace's file again, with the variables it was opened with. */
    bool readWorkspaceFile(const QString &path, QHash<QString, QString> &variables,
                           WorkspaceSpec &spec, bool askForVariables);
    /* A tab for one of a file's windows; null if it has no panes. */
    TerminalTab *makeWindowTab(const WorkspaceSpec &spec, const WorkspaceSpec::Window &window);
    /* Start a window marked `manual` in the current workspace's file. */
    void startManualWindow(const QString &name);
    void saveWorkspaceAs();
    /* Open the current workspace's file in an editor, in a new tab. */
    void editWorkspaceFile();
    /*
     * Run setup commands (before_start, stop) one by one through sh in `dir`,
     * showing progress. False if one fails or is cancelled; `output` gets
     * what they printed.
     */
    bool runSetupCommands(const QStringList &commands, const QString &dir,
                          const QStringList &environment, const QString &what,
                          QString *output);

    /* Workspaces and their tabs at quit, reopened at the next start. */
    void saveSession();
    bool restoreSession();

    /* A new local pane; `directory` overrides the profile's start directory. */
    TerminalWidget *makePane(const TerminalProfile &profile,
                             const QString &directory = QString());
    /* Add a tab to the window and hook up its signals and its first panes. */
    /* `workspace`: an index, or -1 for the shown one. */
    int addTab(Tab *tab, const QString &text, const QString &toolTip,
               int workspace = -1);
    void wireTab(TerminalTab *tab);
    void wirePane(TerminalWidget *pane);
    void wireVncTab(VncTab *tab);
    void wireWebTab(WebTab *tab);
    /* A link from a terminal: a page on this computer in a web tab (where
     * this build has them), anything else in the system's browser. */
    void openLink(const QUrl &url);
    /* Reload the web tabs that ask for it when a command finishes in a
     * terminal of their workspace. */
    void reloadAfterCommands();
    /* Tab text, tooltip and window title: from a terminal tab's active pane,
     * or the remote desktop's name. */
    void syncTabText(Tab *tab);
    /* Whether a key is one of the window's shortcuts (a VNC tab lets those by). */
    bool isAppShortcut(const QKeyEvent *event) const;

    /*
     * tmux. A session attached in control mode shows its windows as tabs and
     * its panes as panes; actions on those become tmux commands. These find
     * the session behind a tab or pane (null for local ones).
     */
    TmuxSession *tmuxSessionFor(const Tab *tab) const;
    TmuxSession *tmuxSessionFor(const TerminalWidget *pane) const;
    /* Before closing a tmux tab or pane: false to leave it open (cancelled,
     * or detached instead because it would end the session). */
    bool confirmTmuxClose(TmuxSession *session, TerminalTab *tab);
    /* Fill the tmux menu: this machine's sessions, then the other actions. */
    void rebuildTmuxMenu();
    /* Attach to (or create) a session through `command`, a control-mode client. */
    /* Its tabs go to the workspace with id `workspaceId` (the shown one if
     * empty); `show` switches to that workspace when the first one arrives. */
    void attachTmux(const QString &label, const QStringList &command,
                    const QString &workspaceId = QString(), bool show = false);
    void newTmuxSession();
    void connectTmuxHost();

    void splitPane(TerminalWidget *pane, Qt::Orientation orientation);
    void closePane(TerminalWidget *pane);
    /* Take a tab out of the window (its panes are gone or going). */
    void removeTab(Tab *tab);
    void syncTabTitles();

    /*
     * Sidebar cards. Previews are redrawn for tabs that have had output since
     * their last one, or for all of them (after a resize, a reorder, a new
     * look); the card's second line and badge are refreshed with them.
     */
    void refreshPreviews(bool all);
    void updateCardDetails(int index);
    /* What the programs in a tab say they are doing (OSC 7501), on its card,
     * its tab and, for the tab on screen, the status bar. */
    void updateProgramStatus(QWidget *page);
    /* A desktop notification when a tab not being looked at needs you, has
     * finished or has failed; taken away once it's seen, or resolved. */
    void notifyProgramStatus(TerminalTab *tab, const ProgramStatusRecord &record);
    static QString notificationKey(const QWidget *tab);
    /* What a notification calls a tab: its name, and its workspace's. */
    QString describeTab(TerminalTab *tab) const;
    /* The sidebar and tab strip take their colours from the current tab's scheme. */
    void updateTabColours();
    void setSidebarWidth(int width);
    void showTabMenu(int index, const QPoint &globalPos);
    /* Name a tab; a tmux tab's window is renamed in tmux. */
    void renameTab(int index);
    void buildMenus();
    void buildStatusBar();
    void buildLayout();
    void openSettings(int section = 0);
    void zoomBy(int halfPoints);

    /*
     * A screenshot goes straight to the clipboard, and the status bar offers
     * to save it too for a few seconds. Nothing is written to disk unasked.
     */
    void onScreenshot(const QPixmap &image);
    void saveScreenshot();

    /*
     * Rebuild the "New Tab with Profile" menu from the store, and push each
     * profile's current definition onto the tabs already open under it.
     */
    void refreshProfiles();

    /*
     * Guard against destroying running work.
     *
     * Returns true if it is safe to quit now. If any terminal is running a
     * foreground process (and the user has not switched the warning off), this
     * asks for confirmation first; the user may cancel, or choose to quit
     * anyway.
     */
    bool confirmQuitIfBusy();

    Settings m_settings;
    ProfileStore m_profiles;
    SettingsDialog *m_settingsDialog = nullptr;

    QSplitter *m_splitter = nullptr;
    ThumbnailSidebar *m_sidebar = nullptr;
    TabStrip *m_tabStrip = nullptr;     // the tabs along the top (QTabBar is hidden)
    QTabWidget *m_tabs = nullptr;

    QLabel *m_statusDir = nullptr;
    QLabel *m_statusInfo = nullptr;
    QLabel *m_screenshotNotice = nullptr;
    QLabel *m_projectNotice = nullptr;
    QString m_projectNoticeDir;    // the directory it was last worked out for
    QString m_projectNoticeFile;
    QTimer *m_screenshotNoticeTimer = nullptr;
    QPixmap m_lastScreenshot;
    QTimer *m_previewTimer = nullptr;
    QMenu *m_newTabWithProfileMenu = nullptr;
    QMenu *m_tmuxMenu = nullptr;
    Notifier *m_notifier = nullptr;
    QList<TmuxSession *> m_tmuxSessions;
    // The workspace (id) each session's tabs belong to.
    QHash<TmuxSession *, QString> m_sessionWorkspace;

    QList<Workspace> m_workspaces;
    int m_currentWorkspace = 0;
    QWidget *m_parking = nullptr;       // hidden parent of parked tabs
    bool m_switchingWorkspace = false;
    QMenu *m_workspaceMenu = nullptr;
    QToolButton *m_workspaceButton = nullptr;
    QAction *m_newWorkspaceAct = nullptr;
    QMenu *m_openWorkspaceMenu = nullptr;
    QMenu *m_startWindowMenu = nullptr;
    QAction *m_saveWorkspaceAct = nullptr;
    QAction *m_editWorkspaceFileAct = nullptr;
    QAction *m_editWorkspaceAct = nullptr;
    QAction *m_closeWorkspaceAct = nullptr;
    QAction *m_nextWorkspaceAct = nullptr;
    QAction *m_prevWorkspaceAct = nullptr;

    QObject *m_moveSource = nullptr;    // the sidebar or strip a drag is moving a tab in

    int m_nextTabId = 1;
};
