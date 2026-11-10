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
    void refreshThumbnail();
    void updateStatus();
    void updateTabLayout();

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

    int m_nextTabId = 1;
};
