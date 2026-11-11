/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QMainWindow>
#include <QVector>

#include "settings.h"

class QTabWidget;
class QSplitter;
class QLabel;
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
    void newTab();
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

    Settings m_settings;
    SettingsDialog *m_settingsDialog = nullptr;

    QSplitter *m_splitter = nullptr;
    ThumbnailSidebar *m_sidebar = nullptr;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_statusDir = nullptr;
    QLabel *m_statusInfo = nullptr;
    QTimer *m_previewTimer = nullptr;

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
