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
class TerminalWidget;
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

private slots:
    /* Open a tab using the default profile. */
    void newTab();
    /* Open a tab using a specific profile id; falls back to the default. */
    void newTabWithProfile(const QString &profileId);
    void closeTab(int index);
    void applyAppearanceToAll();
    void updateStatus();
    void updateTabLayout();

    /* Capture one tab's preview, rotating through all tabs over time. */
    void captureNextPreview();

private:
    TerminalWidget *currentTerminal() const;
    TerminalWidget *terminalAt(int index) const;
    void syncTabTitles();
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

    /*
     * Preview rotation.
     *
     * A widget only has pixels once it has painted, and a hidden QTabWidget
     * page never paints: grabbing one yields a blank image (measured -- a
     * hidden page also collapses to 100x30 until laid out). To preview
     * inactive tabs the app briefly brings each one to the front, lets it
     * paint, grabs it, and restores the user's tab. The dwell is short enough
     * to be visually negligible but must span an event-loop turn.
     */
    int m_previewTarget = -1;      // tab currently being captured
    int m_restoreTab = -1;         // tab to return to afterwards
    bool m_capturingPreview = false;
    int m_nextPreviewTab = 0;      // rotation cursor

    int m_nextTabId = 1;
};
