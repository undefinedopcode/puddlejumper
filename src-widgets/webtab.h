/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "tab.h"

#include <QUrl>

#include <functional>

class QLineEdit;
class QSplitter;
class QTimer;
class QToolButton;
class QWebEngineProfile;
class QWebEngineView;

/*
 * A tab showing a web page: the app being worked on, next to the terminals
 * building it. A toolbar (back, forward, reload, the address, DevTools) and
 * the page; its card shows the page as last seen.
 */
class WebTab : public Tab
{
    Q_OBJECT

public:
    explicit WebTab(const QUrl &url = QUrl(), QWidget *parent = nullptr);

    QUrl url() const;
    void load(const QUrl &url);
    void reload();
    /* The page's title, or its address until it has one. */
    QString title() const;
    /* The address, for the status bar and the card. */
    QString details() const;

    /* Reloaded when a command in a terminal of its workspace finishes (a
     * build, say); kept with the tab. */
    bool reloadAfterCommands() const { return m_reloadAfterCommands; }
    void setReloadAfterCommands(bool on);

    /* Chromium's developer tools, beside the page. */
    bool devToolsShown() const;
    void setDevToolsShown(bool shown);

    /* Where a page's new windows (a link to _blank, window.open()) go: the
     * view of a tab made for them, or none to load them here. */
    void setNewTabHandler(std::function<WebTab *()> handler) { m_newTab = std::move(handler); }
    QWebEngineView *view() const { return m_view; }

    /* What the address bar makes of typed text: "localhost:3000" and
     * "example.com/x" are http(s) addresses. */
    static QUrl urlFromInput(const QString &text);
    /* Whether a page is served from this computer (localhost and the like),
     * as a web tab's are, rather than from the internet. */
    static bool isLocal(const QUrl &url);
    /* The pages' cookies, storage and cache, shared by every web tab and kept
     * between runs. */
    static QWebEngineProfile *profile();

    bool hasNewOutput() const override { return m_newOutput; }
    void clearNewOutput() override { m_newOutput = false; }
    QPixmap previewPixmap(const QSize &size, qreal devicePixelRatio = 1.0) const override;
    QColor backgroundColour() const override;
    QColor foregroundColour() const override;
    void focusContent() override;

signals:
    /* The title, address, icon or loading state changed. */
    void changed();

private:
    friend class WebTabView;

    void updateControls();
    /* The page as it looks now, for the card (only while it is on screen:
     * a hidden view draws nothing). */
    void takeSnapshot();

    QWebEngineView *m_view = nullptr;
    QWebEngineView *m_devTools = nullptr;
    QSplitter *m_splitter = nullptr;
    QLineEdit *m_address = nullptr;
    QToolButton *m_back = nullptr;
    QToolButton *m_forward = nullptr;
    QToolButton *m_reload = nullptr;
    QToolButton *m_devToolsButton = nullptr;
    QTimer *m_snapshotTimer = nullptr;
    QUrl m_requested;   // the last address loaded
    QPixmap m_snapshot;
    bool m_loading = false;
    bool m_newOutput = true;
    bool m_reloadAfterCommands = false;
    std::function<WebTab *()> m_newTab;
};
