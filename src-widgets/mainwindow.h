/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QMainWindow>
#include <QPixmap>
#include <QVector>

#include "profiles.h"
#include "settings.h"

class QTabWidget;
class QSplitter;
class QLabel;
class QMenu;
class QTimer;
class TerminalTab;
class TerminalWidget;
class TmuxSession;
class SettingsDialog;
class ThumbnailSidebar;

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

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /* Open a tab using the default profile. */
    void newTab();
    /* Open a tab using a specific profile id; falls back to the default. */
    void newTabWithProfile(const QString &profileId);
    void closeTab(int index);
    void applyAppearanceToAll();
    void updateStatus();
    void updateTabLayout();

private:
    /* The active pane of the current tab: what window actions act on. */
    TerminalWidget *currentTerminal() const;
    TerminalTab *currentTab() const;
    TerminalTab *tabAt(int index) const;
    /* The tab holding `pane`, and its index (-1 if none). */
    TerminalTab *tabOf(const QWidget *pane) const;
    int tabIndexOf(const QWidget *pane) const;
    /* Every pane in every tab. */
    QList<TerminalWidget *> allPanes() const;

    /* A new local pane; `directory` overrides the profile's start directory. */
    TerminalWidget *makePane(const TerminalProfile &profile,
                             const QString &directory = QString());
    /* Add a tab to the window and hook up its signals and its first panes. */
    int addTab(TerminalTab *tab, const QString &text, const QString &toolTip);
    void wireTab(TerminalTab *tab);
    void wirePane(TerminalWidget *pane);
    /* Tab text, tooltip and window title from the tab's active pane. */
    void syncTabFromActivePane(TerminalTab *tab);

    /*
     * tmux. A session attached in control mode shows its windows as tabs and
     * its panes as panes; actions on those become tmux commands. These find
     * the session behind a tab or pane (null for local ones).
     */
    TmuxSession *tmuxSessionFor(const TerminalTab *tab) const;
    TmuxSession *tmuxSessionFor(const TerminalWidget *pane) const;
    /* Before closing a tmux tab or pane: false to leave it open (cancelled,
     * or detached instead because it would end the session). */
    bool confirmTmuxClose(TmuxSession *session, TerminalTab *tab);
    /* Fill the tmux menu: this machine's sessions, then the other actions. */
    void rebuildTmuxMenu();
    /* Attach to (or create) a session through `command`, a control-mode client. */
    void attachTmux(const QString &label, const QStringList &command);
    void newTmuxSession();
    void connectTmuxHost();

    void splitPane(TerminalWidget *pane, Qt::Orientation orientation);
    void closePane(TerminalWidget *pane);
    /* Take a tab out of the window (its panes are gone or going). */
    void removeTab(TerminalTab *tab);
    void syncTabTitles();

    /*
     * Sidebar cards. Previews are redrawn for tabs that have had output since
     * their last one, or for all of them (after a resize, a reorder, a new
     * look); the card's second line and badge are refreshed with them.
     */
    void refreshPreviews(bool all);
    void updateCardDetails(int index);
    /* The sidebar takes its colours from the current tab's scheme. */
    void updateSidebarColours();
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
    QTabWidget *m_tabs = nullptr;

    QLabel *m_statusDir = nullptr;
    QLabel *m_statusInfo = nullptr;
    QLabel *m_screenshotNotice = nullptr;
    QTimer *m_screenshotNoticeTimer = nullptr;
    QPixmap m_lastScreenshot;
    QTimer *m_previewTimer = nullptr;
    QMenu *m_newTabWithProfileMenu = nullptr;
    QMenu *m_tmuxMenu = nullptr;
    QList<TmuxSession *> m_tmuxSessions;

    bool m_movingFromSidebar = false;   // a card drag is moving a tab

    int m_nextTabId = 1;
};
