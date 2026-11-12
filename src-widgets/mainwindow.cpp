/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "settingsdialog.h"
#include "terminaltab.h"
#include "terminalwidget.h"
#include "themeicon.h"
#include "thumbnailsidebar.h"
#include "tmuxcontrol.h"
#include "tmuxsession.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QResizeEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QProcess>
#include <QDialogButtonBox>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <Emulation.h>
#include <qtermwidget.h>

namespace {

/*
 * Toolbar buttons are icon-only, so the tooltip is the only place a button's
 * name and shortcut appear. Qt doesn't add the shortcut itself.
 */
/*
 * How to run tmux. SUPERTERM_TMUX overrides it, e.g. "tmux -L test" to use a
 * separate server (handy for testing without touching real sessions).
 */
QStringList tmuxProgram()
{
    const QString env = QString::fromLocal8Bit(qgetenv("SUPERTERM_TMUX")).trimmed();
    const QStringList cmd = env.isEmpty() ? QStringList() : QProcess::splitCommand(env);
    return cmd.isEmpty() ? QStringList{QStringLiteral("tmux")} : cmd;
}

/* Quote for a POSIX shell (the remote one, for ssh). */
QString shellQuote(const QString &arg)
{
    QString out = arg;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

constexpr int kDefaultSidebarWidth = 220;

/*
 * The handle between the sidebar and the terminals: painted in the sidebar's
 * own colour, with a faint line on hover so it can be found. Double-click
 * puts the sidebar back to its default width.
 */
class SidebarHandle : public QSplitterHandle
{
public:
    SidebarHandle(Qt::Orientation orientation, QSplitter *parent,
                  const ThumbnailSidebar *sidebar, std::function<void()> reset)
        : QSplitterHandle(orientation, parent), m_sidebar(sidebar), m_reset(std::move(reset))
    {
        setAttribute(Qt::WA_Hover);
        setToolTip(QObject::tr("Drag to resize, or to the edge to hide; double-click to reset"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QColor bg = m_sidebar->background();
        p.fillRect(rect(), bg);
        if (underMouse()) {
            QColor line = m_sidebar->palette().color(QPalette::WindowText);
            line.setAlphaF(0.35);
            p.fillRect(QRect(width() / 2 - 1, 0, 2, height()), line);
        }
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        return QSplitterHandle::event(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *) override { m_reset(); }

private:
    const ThumbnailSidebar *m_sidebar;
    std::function<void()> m_reset;
};

class SidebarSplitter : public QSplitter
{
public:
    SidebarSplitter(QWidget *parent, std::function<const ThumbnailSidebar *()> sidebar,
                    std::function<void()> reset)
        : QSplitter(Qt::Horizontal, parent), m_sidebar(std::move(sidebar)), m_reset(std::move(reset)) {}

protected:
    QSplitterHandle *createHandle() override
    {
        return new SidebarHandle(orientation(), this, m_sidebar(), m_reset);
    }

private:
    std::function<const ThumbnailSidebar *()> m_sidebar;
    std::function<void()> m_reset;
};

/* "~/src/x" rather than "/home/me/src/x". */
QString tildePath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

void setToolTipWithShortcut(QAction *act)
{
    QString text = act->text();
    text.remove(QLatin1Char('&'));
    text.remove(QChar(0x2026));   // "…"
    const QString keys = act->shortcut().toString(QKeySequence::NativeText);
    act->setToolTip(keys.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, keys));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SuperTerm"));
    setMinimumSize(760, 420);

    buildLayout();
    buildMenus();
    buildStatusBar();

    // Live previews: a few times a second, redraw the cards of tabs that
    // have had output since. Previews are drawn from the panes' screens, so
    // hidden tabs update as well as the visible one, and idle tabs cost nothing.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, [this]() { refreshPreviews(false); });
    m_previewTimer->start();

    connect(&m_settings, &Settings::appearanceChanged,
            this, &MainWindow::applyAppearanceToAll);
    connect(&m_settings, &Settings::scrollingChanged, this, [this]() {
        for (TerminalWidget *t : allPanes())
            t->applyScrolling();
    });

    // Any profile edit: rebuild the menu and refresh live tabs that are still
    // open under that profile.
    connect(&m_profiles, &ProfileStore::profilesChanged,
            this, &MainWindow::refreshProfiles);

    // Restore geometry from the previous session.
    if (!m_settings.windowGeometry().isEmpty())
        restoreGeometry(m_settings.windowGeometry());
    else
        resize(1100, 650);
    if (!m_settings.windowState().isEmpty())
        restoreState(m_settings.windowState());

    refreshProfiles();   // populate the submenu before the first tab opens
    newTab();
    updateTabLayout();
}

void MainWindow::buildLayout()
{
    m_splitter = new SidebarSplitter(
        this, [this]() { return m_sidebar; },
        [this]() { setSidebarWidth(kDefaultSidebarWidth); });
    m_splitter->setHandleWidth(5);

    m_sidebar = new ThumbnailSidebar(m_splitter);

    m_tabs = new QTabWidget(m_splitter);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);
    // Its size gives the previews their shape.
    m_tabs->installEventFilter(this);

    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_tabs);
    setCentralWidget(m_splitter);

    // The terminals take any change in window width; the sidebar keeps its
    // own, which is remembered. Dragging it into the edge hides it, like
    // View > Thumbnail Sidebar.
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);
    const int saved = m_settings.sidebarWidth();
    setSidebarWidth(saved > 0 ? saved : kDefaultSidebarWidth);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        const int width = m_splitter->sizes().value(0);
        if (width == 0) {
            m_settings.setTabLayout(QStringLiteral("standard"));
            return;
        }
        m_settings.setSidebarWidth(width);
    });

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        m_sidebar->setCurrentIndex(index);
        if (auto *tab = tabAt(index)) {
            // Keep tmux's current window in step, so another client attaching
            // sees the same one.
            if (auto *session = tmuxSessionFor(tab))
                session->selectWindow(tab);
            // Selecting a tab counts as having seen its output.
            m_sidebar->setBusy(index, false);
            if (tab->activePane())
                tab->activePane()->focusTerminal();
            syncTabFromActivePane(tab);
        }
        updateSidebarColours();
        updateStatus();
    });
    // A tab dragged along the tab bar: move its card too. (A card dragged in
    // the sidebar moves the tab, below, and the card is already in place.)
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (!m_movingFromSidebar)
            m_sidebar->moveCard(from, to);
    });

    connect(m_sidebar, &ThumbnailSidebar::tabActivated, this, [this](int index) {
        if (index >= 0 && index < m_tabs->count() && index != m_tabs->currentIndex())
            m_tabs->setCurrentIndex(index);
    });
    connect(m_sidebar, &ThumbnailSidebar::tabClosed, this, &MainWindow::closeTab);
    connect(m_sidebar, &ThumbnailSidebar::tabMoved, this, [this](int from, int to) {
        m_movingFromSidebar = true;
        m_tabs->tabBar()->moveTab(from, to);
        m_movingFromSidebar = false;
    });
    connect(m_sidebar, &ThumbnailSidebar::newTabRequested, this, &MainWindow::newTab);
    connect(m_sidebar, &ThumbnailSidebar::contextMenuRequested,
            this, &MainWindow::showTabMenu);
    connect(m_sidebar, &ThumbnailSidebar::previewSizeChanged,
            this, [this]() { refreshPreviews(true); });
}

void MainWindow::setSidebarWidth(int width)
{
    const int total = qMax(m_splitter->width(), width + 200);
    m_splitter->setSizes({width, total - width});
    m_settings.setSidebarWidth(width);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabs && event->type() == QEvent::Resize) {
        // Cards take the terminal area's shape: the tab widget less its tab
        // bar. (The pages haven't been resized yet when this runs.)
        QSize area = static_cast<QResizeEvent *>(event)->size();
        if (m_tabs->tabBar()->isVisible())
            area.rheight() -= m_tabs->tabBar()->sizeHint().height();
        if (area.width() > 0 && area.height() > 0)
            m_sidebar->setPreviewAspect(qreal(area.height()) / area.width());
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildMenus()
{
    /*
     * Shortcut policy.
     *
     * A QAction shortcut is claimed by the shortcut map before the focused
     * widget -- the terminal -- ever sees the key. Anything a terminal program
     * might want is therefore off limits to the app, however conventional it
     * looks as a desktop shortcut.
     *
     * Plain Ctrl+letter is the terminal's: those are the control codes programs
     * are built around (Ctrl+C, Ctrl+Q, Ctrl+S, Ctrl+Z, Ctrl+D...). The app
     * keeps nothing there. Ctrl+Q in particular was bound to Quit via
     * QKeySequence::Quit, which swallowed micro's quit key and killed the app
     * instead.
     *
     * Chrome navigation may use plain Ctrl only where a terminal program has no
     * use for the combination -- Ctrl+Tab, Ctrl+Comma, Ctrl+Equal/Minus/0 --
     * or Ctrl+Shift+letter for anything else. This is the same split Konsole
     * makes.
     */
    auto *file = menuBar()->addMenu(tr("&File"));

    auto *newAct = file->addAction(tr("&New Tab"));
    newAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    connect(newAct, &QAction::triggered, this, &MainWindow::newTab);

    // Populated by refreshProfiles(), which runs whenever the store changes.
    // A submenu rather than N top-level actions keeps the File menu stable as
    // profiles come and go.
    m_newTabWithProfileMenu = file->addMenu(tr("New Tab &with Profile"));

    auto *closeAct = file->addAction(tr("&Close Tab"));
    closeAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(closeAct, &QAction::triggered, this, [this]() { closeTab(m_tabs->currentIndex()); });

    auto *renameAct = file->addAction(tr("&Rename Tab…"));
    connect(renameAct, &QAction::triggered, this, [this]() { renameTab(m_tabs->currentIndex()); });

    auto *nextAct = file->addAction(tr("&Next Tab"));
    nextAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")));
    connect(nextAct, &QAction::triggered, this, [this]() {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % qMax(1, m_tabs->count()));
    });

    auto *prevAct = file->addAction(tr("&Previous Tab"));
    prevAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
    connect(prevAct, &QAction::triggered, this, [this]() {
        const int n = qMax(1, m_tabs->count());
        m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + n) % n);
    });

    file->addSeparator();
    auto *settingsAct = file->addAction(tr("&Settings…"));
    settingsAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAct, &QAction::triggered, this, [this]() { openSettings(); });

    file->addSeparator();
    auto *quitAct = file->addAction(tr("&Quit"));
    // NOT QKeySequence::Quit -- that is plain Ctrl+Q on Linux, and a QAction
    // shortcut is consumed by the shortcut map before the focused widget sees
    // the key. Ctrl+Q belongs to the program in the terminal (it is micro's
    // quit, and a shell's XON resume), so it must never be an app accelerator.
    // See the shortcut policy in buildMenus() above.
    quitAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));
    // Route through close() rather than straight to QApplication::quit(), so
    // this path runs the same "programs are still running" guard as the window
    // close button. close() delivers a QCloseEvent, which closeEvent() vets.
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    auto *edit = menuBar()->addMenu(tr("&Edit"));
    auto *copyAct = edit->addAction(tr("&Copy"));
    copyAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->terminal()->copyClipboard();
    });

    auto *pasteAct = edit->addAction(tr("&Paste"));
    pasteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->terminal()->pasteClipboard();
    });

    edit->addSeparator();
    auto *findAct = edit->addAction(tr("&Find…"));
    findAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(findAct, &QAction::triggered, this, [this]() {
        // Built into QTermWidget -- no overlay to implement.
        if (auto *t = currentTerminal())
            t->terminal()->toggleShowSearchBar();
    });

    edit->addSeparator();
    auto *shotRegionAct = edit->addAction(tr("Screenshot &Region…"));
    shotRegionAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(shotRegionAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->startRegionScreenshot();
    });

    auto *shotTermAct = edit->addAction(tr("Screens&hot Terminal"));
    shotTermAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(shotTermAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->takeScreenshot();
    });

    auto *view = menuBar()->addMenu(tr("&View"));
    // Filled each time it opens, since tmux sessions come and go outside.
    m_tmuxMenu = menuBar()->addMenu(tr("t&mux"));
    connect(m_tmuxMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTmuxMenu);

    auto *zoomIn = view->addAction(tr("Zoom &In"));
    zoomIn->setShortcut(QKeySequence(QStringLiteral("Ctrl+=")));
    connect(zoomIn, &QAction::triggered, this, [this]() { zoomBy(+1); });

    auto *zoomOut = view->addAction(tr("Zoom &Out"));
    zoomOut->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
    connect(zoomOut, &QAction::triggered, this, [this]() { zoomBy(-1); });

    auto *zoomReset = view->addAction(tr("&Reset Zoom"));
    zoomReset->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(zoomReset, &QAction::triggered, this, [this]() {
        m_settings.setFontSize(13.0);
    });

    view->addSeparator();
    auto *splitRightAct = view->addAction(tr("Split &Right"));
    splitRightAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(splitRightAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            splitPane(t, Qt::Horizontal);
    });
    auto *splitDownAct = view->addAction(tr("Split &Down"));
    splitDownAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(splitDownAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            splitPane(t, Qt::Vertical);
    });
    auto *closePaneAct = view->addAction(tr("Close Pa&ne"));
    closePaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+X")));
    connect(closePaneAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            closePane(t);
    });
    auto *nextPaneAct = view->addAction(tr("Ne&xt Pane"));
    nextPaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connect(nextPaneAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->cyclePane(+1);
    });
    auto *prevPaneAct = view->addAction(tr("Pre&vious Pane"));
    prevPaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")));
    connect(prevPaneAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->cyclePane(-1);
    });

    view->addSeparator();
    auto *toggleLayout = view->addAction(tr("&Thumbnail Sidebar"));
    toggleLayout->setCheckable(true);
    toggleLayout->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    connect(toggleLayout, &QAction::triggered, this, [this](bool on) {
        m_settings.setTabLayout(on ? QStringLiteral("thumbnails")
                                   : QStringLiteral("standard"));
    });
    // Keep the check state in step with the setting, however it changed, and
    // start it from the saved setting (which emits nothing at startup).
    const auto syncLayoutCheck = [this, toggleLayout]() {
        toggleLayout->setChecked(
            m_settings.appearance().tabLayout == QStringLiteral("thumbnails"));
    };
    syncLayoutCheck();
    connect(&m_settings, &Settings::appearanceChanged, this, syncLayoutCheck);

    /*
     * Toolbar.
     *
     * The same QActions as the menus, so shortcuts, enabled state and the
     * sidebar check stay in one place. Its visibility and position are part of
     * saveState(), which closeEvent() persists; that needs the object name.
     */
    newAct->setIcon(themeIcon({"tab-new", "list-add"}));
    closeAct->setIcon(themeIcon({"tab-close", "window-close"}));
    splitRightAct->setIcon(themeIcon({"view-split-left-right"}));
    splitDownAct->setIcon(themeIcon({"view-split-top-bottom"}));
    closePaneAct->setIcon(themeIcon({"view-close", "window-close"}));
    copyAct->setIcon(themeIcon({"edit-copy"}));
    pasteAct->setIcon(themeIcon({"edit-paste"}));
    findAct->setIcon(themeIcon({"edit-find", "system-search"}));
    shotRegionAct->setIcon(themeIcon({"camera-photo", "applets-screenshooter"}));
    shotTermAct->setIcon(themeIcon({"view-fullscreen"}));
    zoomOut->setIcon(themeIcon({"zoom-out"}));
    zoomIn->setIcon(themeIcon({"zoom-in"}));
    toggleLayout->setIcon(themeIcon({"sidebar-show", "sidebar-show-symbolic", "view-preview"}));
    settingsAct->setIcon(themeIcon({"configure", "preferences-system", "document-properties"}));
    for (QAction *act : {newAct, closeAct, splitRightAct, splitDownAct, closePaneAct,
                         nextPaneAct, prevPaneAct, copyAct, pasteAct, findAct,
                         shotRegionAct, shotTermAct, zoomOut, zoomIn, zoomReset,
                         toggleLayout, settingsAct})
        setToolTipWithShortcut(act);

    auto *bar = addToolBar(tr("Main Toolbar"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setFloatable(false);

    // New Tab with the profile list as a drop-down, rather than a second
    // button: clicking opens the default profile, the arrow picks another.
    auto *newButton = new QToolButton(bar);
    newButton->setDefaultAction(newAct);
    newButton->setMenu(m_newTabWithProfileMenu);
    newButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(newButton);
    bar->addAction(closeAct);

    bar->addSeparator();
    bar->addAction(splitRightAct);
    bar->addAction(splitDownAct);

    bar->addSeparator();
    bar->addAction(copyAct);
    bar->addAction(pasteAct);
    bar->addAction(findAct);

    // One screenshot button: a click selects a region, the arrow offers the
    // whole terminal as well.
    auto *shotMenu = new QMenu(this);
    shotMenu->addAction(shotRegionAct);
    shotMenu->addAction(shotTermAct);
    auto *shotButton = new QToolButton(bar);
    shotButton->setDefaultAction(shotRegionAct);
    shotButton->setMenu(shotMenu);
    shotButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(shotButton);

    // The current font size sits between the zoom buttons, and clicking it
    // resets the zoom. Not setDefaultAction(): that would replace the size with
    // the action's text.
    bar->addSeparator();
    bar->addAction(zoomOut);
    auto *sizeButton = new QToolButton(bar);
    sizeButton->setToolTip(zoomReset->toolTip());
    sizeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(sizeButton, &QToolButton::clicked, zoomReset, &QAction::trigger);
    const auto showFontSize = [this, sizeButton]() {
        sizeButton->setText(tr("%L1 pt").arg(m_settings.appearance().fontSize));
    };
    showFontSize();
    connect(&m_settings, &Settings::appearanceChanged, sizeButton, showFontSize);
    bar->addWidget(sizeButton);
    bar->addAction(zoomIn);

    bar->addSeparator();
    bar->addAction(toggleLayout);

    // Settings at the far end, as in most desktop apps.
    auto *spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    bar->addAction(settingsAct);

    view->addSeparator();
    QAction *showBar = bar->toggleViewAction();
    showBar->setText(tr("Show &Toolbar"));
    view->addAction(showBar);
}

void MainWindow::buildStatusBar()
{
    m_statusDir = new QLabel(this);
    m_statusInfo = new QLabel(this);
    statusBar()->addWidget(m_statusDir, 1);

    m_screenshotNotice = new QLabel(this);
    m_screenshotNotice->setTextFormat(Qt::RichText);
    m_screenshotNotice->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_screenshotNotice->setFocusPolicy(Qt::NoFocus);
    m_screenshotNotice->hide();
    connect(m_screenshotNotice, &QLabel::linkActivated, this, &MainWindow::saveScreenshot);
    statusBar()->addPermanentWidget(m_screenshotNotice);

    m_screenshotNoticeTimer = new QTimer(this);
    m_screenshotNoticeTimer->setSingleShot(true);
    m_screenshotNoticeTimer->setInterval(10000);
    connect(m_screenshotNoticeTimer, &QTimer::timeout, m_screenshotNotice, &QWidget::hide);

    statusBar()->addPermanentWidget(m_statusInfo);
}

void MainWindow::onScreenshot(const QPixmap &image)
{
    if (image.isNull())
        return;
    m_lastScreenshot = image;
    QApplication::clipboard()->setImage(image.toImage());

    const QSize px = image.size();   // device pixels, i.e. what gets saved
    m_screenshotNotice->setText(
        tr("Screenshot copied to clipboard (%1 × %2) · <a href=\"save\">Save…</a>")
            .arg(px.width()).arg(px.height()));
    m_screenshotNotice->show();
    m_screenshotNoticeTimer->start();
}

void MainWindow::saveScreenshot()
{
    if (m_lastScreenshot.isNull())
        return;
    m_screenshotNoticeTimer->stop();   // don't vanish while the dialog is open

    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::homePath();
    const QString suggested = dir + QStringLiteral("/superterm-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))
        + QStringLiteral(".png");

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Screenshot"), suggested,
        tr("PNG images (*.png);;JPEG images (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        m_screenshotNoticeTimer->start();
        return;
    }
    // QPixmap::save() picks the format from the suffix, so a bare name
    // would fail; default those to PNG.
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");
    if (!m_lastScreenshot.save(path)) {
        QMessageBox::warning(this, tr("Save Screenshot"),
                             tr("Could not save the screenshot to %1.").arg(path));
        m_screenshotNoticeTimer->start();
        return;
    }
    m_screenshotNotice->setText(tr("Screenshot saved to %1").arg(path.toHtmlEscaped()));
    m_screenshotNoticeTimer->start();
}

TerminalTab *MainWindow::currentTab() const
{
    return qobject_cast<TerminalTab *>(m_tabs->currentWidget());
}

TerminalTab *MainWindow::tabAt(int index) const
{
    if (index < 0 || index >= m_tabs->count())
        return nullptr;
    return qobject_cast<TerminalTab *>(m_tabs->widget(index));
}

TerminalWidget *MainWindow::currentTerminal() const
{
    auto *tab = currentTab();
    return tab ? tab->activePane() : nullptr;
}

TerminalTab *MainWindow::tabOf(const QWidget *pane) const
{
    for (const QWidget *w = pane; w; w = w->parentWidget()) {
        if (auto *tab = qobject_cast<TerminalTab *>(const_cast<QWidget *>(w)))
            return tab;
    }
    return nullptr;
}

int MainWindow::tabIndexOf(const QWidget *pane) const
{
    auto *tab = tabOf(pane);
    return tab ? m_tabs->indexOf(tab) : -1;
}

QList<TerminalWidget *> MainWindow::allPanes() const
{
    QList<TerminalWidget *> out;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *tab = tabAt(i))
            out += tab->panes();
    }
    return out;
}

TerminalWidget *MainWindow::makePane(const TerminalProfile &profile,
                                     const QString &directory)
{
    TerminalProfile p = profile;
    if (!directory.isEmpty())
        p.workingDirectory = directory;
    auto *pane = new TerminalWidget(m_nextTabId++, m_settings, p, this);
    pane->applyAppearance();
    pane->applyScrolling();
    return pane;
}

int MainWindow::addTab(TerminalTab *tab, const QString &text, const QString &toolTip)
{
    const int index = m_tabs->addTab(tab, text);
    m_tabs->setTabToolTip(index, toolTip);
    wireTab(tab);
    for (TerminalWidget *pane : tab->panes())
        wirePane(pane);
    syncTabTitles();
    return index;
}

void MainWindow::wireTab(TerminalTab *tab)
{
    connect(tab, &TerminalTab::paneAdded, this, &MainWindow::wirePane);
    connect(tab, &TerminalTab::activePaneChanged, this, [this, tab]() {
        syncTabFromActivePane(tab);
        if (tab == currentTab())
            updateStatus();
    });
    // The last pane closed (its shell exited, or Close Pane): the tab goes too.
    connect(tab, &TerminalTab::empty, this, [this, tab]() { removeTab(tab); });
}

void MainWindow::syncTabFromActivePane(TerminalTab *tab)
{
    const int i = m_tabs->indexOf(tab);
    TerminalWidget *pane = tab ? tab->activePane() : nullptr;
    if (i < 0 || !pane)
        return;
    const QString title = pane->sessionTitle();

    // A tmux tab is named after its window, as in tmux itself; the active
    // pane's title goes in the tooltip and window title.
    if (auto *session = tmuxSessionFor(tab)) {
        const QString name = session->windowName(tab);
        m_tabs->setTabText(i, name);
        m_sidebar->setTitle(i, name);
        m_tabs->setTabToolTip(i, title.isEmpty()
                                     ? tr("tmux %1 — %2").arg(session->label(), name)
                                     : tr("tmux %1 — %2\n%3").arg(session->label(), name, title));
        if (tab == currentTab())
            setWindowTitle(tr("%1 — tmux %2 — SuperTerm")
                               .arg(title.isEmpty() ? name : title, session->label()));
        return;
    }

    // A name the user gave the tab wins over whatever the shell calls it.
    const QString name = !tab->customTitle().isEmpty() ? tab->customTitle()
                         : !title.isEmpty()            ? title
                                                       : tr("Terminal");
    m_tabs->setTabText(i, name);
    m_tabs->setTabToolTip(i, title.isEmpty() || title == name
                                 ? name : QStringLiteral("%1\n%2").arg(name, title));
    m_sidebar->setTitle(i, name);
    if (tab == currentTab())
        setWindowTitle(title.isEmpty() ? QStringLiteral("SuperTerm")
                                       : tr("%1 — SuperTerm").arg(title));
}

void MainWindow::wirePane(TerminalWidget *term)
{
    // A pane's title names its tab while it is the tab's active pane.
    connect(term, &TerminalWidget::titleChanged, this, [this, term]() {
        auto *tab = tabOf(term);
        if (tab && tab->activePane() == term)
            syncTabFromActivePane(tab);
    });

    connect(term, &TerminalWidget::sessionFinished, this, [this, term]() {
        closePane(term);
    });

    connect(term, &TerminalWidget::splitRequested, this, [this, term](Qt::Orientation o) {
        splitPane(term, o);
    });
    connect(term, &TerminalWidget::closePaneRequested, this, [this, term]() {
        closePane(term);
    });

    // Mark a tab as having new output when it is not the one on screen. This is
    // the visible "something changed here" cue, and it also tells the preview
    // rotation which tab is worth re-capturing.
    connect(term, &TerminalWidget::outputArrived, this, [this, term]() {
        const int i = tabIndexOf(term);
        if (i >= 0 && i != m_tabs->currentIndex())
            m_sidebar->setBusy(i, true);
    });

    connect(term, &TerminalWidget::screenshotTaken, this, &MainWindow::onScreenshot);

    // The context menu zoom requests apply to the shared Settings, so every
    // tab updates at once (and the choice is persisted).
    connect(term, &TerminalWidget::zoomInRequested,    this, [this]() { zoomBy(+1); });
    connect(term, &TerminalWidget::zoomOutRequested,   this, [this]() { zoomBy(-1); });
    connect(term, &TerminalWidget::zoomResetRequested, this, [this]() {
        m_settings.setFontSize(13.0);
    });

    // Output monitoring (activity / silence). Both reuse the existing busy
    // marker: the sidebar prefix-dot is cheap, survives, and clears on tab
    // select, which is exactly the affordance "come back when the build is
    // done" wants. The signals only fire while the matching setting is on.
    auto markIfBackground = [this, term]() {
        const int i = tabIndexOf(term);
        if (i >= 0 && i != m_tabs->currentIndex())
            m_sidebar->setBusy(i, true);
    };
    connect(term, &TerminalWidget::activityDetected, this, markIfBackground);
    connect(term, &TerminalWidget::silenceDetected,  this, markIfBackground);
}

void MainWindow::splitPane(TerminalWidget *pane, Qt::Orientation orientation)
{
    if (auto *session = tmuxSessionFor(pane)) {
        session->splitPane(pane, orientation);
        return;
    }
    auto *tab = tabOf(pane);
    if (!tab)
        return;
    // The new pane opens with the same profile, in the directory the shell
    // beside it is in now -- usually what you want next to you.
    tab->splitPane(pane, orientation,
                   makePane(pane->profile(), pane->currentDirectory()));
}

void MainWindow::closePane(TerminalWidget *pane)
{
    if (auto *session = tmuxSessionFor(pane)) {
        if (!confirmTmuxClose(session, tabOf(pane)))
            return;
        session->killPane(pane);   // the layout change removes it
        return;
    }
    if (auto *tab = tabOf(pane))
        tab->removePane(pane);   // the last pane takes the tab with it (empty())
}

void MainWindow::removeTab(TerminalTab *tab)
{
    const int index = m_tabs->indexOf(tab);
    if (index < 0)
        return;
    m_tabs->removeTab(index);
    tab->deleteLater();

    if (m_tabs->count() == 0) {
        newTab();
        return;
    }
    syncTabTitles();
    updateStatus();
}

void MainWindow::newTab()
{
    // On a tmux tab, a new tab is a new tmux window; New Tab with Profile
    // always opens a local one.
    if (auto *session = tmuxSessionFor(currentTab())) {
        session->newWindow();
        return;
    }
    newTabWithProfile(QString());
}

TmuxSession *MainWindow::tmuxSessionFor(const TerminalTab *tab) const
{
    for (TmuxSession *s : m_tmuxSessions) {
        if (s->ownsTab(tab))
            return s;
    }
    return nullptr;
}

TmuxSession *MainWindow::tmuxSessionFor(const TerminalWidget *pane) const
{
    for (TmuxSession *s : m_tmuxSessions) {
        if (s->ownsPane(pane))
            return s;
    }
    return nullptr;
}

void MainWindow::rebuildTmuxMenu()
{
    m_tmuxMenu->clear();

    // Sessions on this machine. A quick synchronous query: tmux answers in
    // milliseconds, and the menu has to know before it opens.
    QProcess list;
    const QStringList program = tmuxProgram();
    list.start(program.first(), program.mid(1) + QStringList{
        QStringLiteral("list-sessions"), QStringLiteral("-F"),
        QStringLiteral("#{session_name}\t#{session_windows}\t#{session_attached}")});
    const bool started = list.waitForStarted(2000);
    if (started)
        list.waitForFinished(2000);

    if (!started) {
        m_tmuxMenu->addAction(tr("tmux is not installed"))->setEnabled(false);
        return;
    }

    const QList<QByteArray> lines = list.readAllStandardOutput().split('\n');
    int listed = 0;
    for (const QByteArray &line : lines) {
        const QList<QByteArray> f = line.split('\t');
        if (f.size() < 3)
            continue;
        const QString name = QString::fromUtf8(f.at(0));
        const int windows = f.at(1).toInt();
        bool ours = false;
        for (TmuxSession *s : std::as_const(m_tmuxSessions))
            ours = ours || (s->label() == name && !s->tabs().isEmpty());
        QString text = tr("%1  (%n window(s))", nullptr, windows).arg(name);
        if (ours)
            text += tr(" — open here");
        else if (f.at(2).toInt() > 0)
            text += tr(" — attached elsewhere");
        QAction *act = m_tmuxMenu->addAction(text);
        // Decided when chosen, not now: a global menu (Plasma's, over D-Bus)
        // can show a copy of this menu from before a detach or attach.
        // attachTmux() goes to the session's tabs if it is open here already.
        connect(act, &QAction::triggered, this, [this, name]() {
            qCInfo(lcTmux) << "menu: open" << name;
            attachTmux(name, tmuxProgram() + QStringList{
                QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("attach-session"),
                QStringLiteral("-t"), name});
        });
        ++listed;
    }
    if (listed == 0)
        m_tmuxMenu->addAction(tr("No sessions running"))->setEnabled(false);

    m_tmuxMenu->addSeparator();
    m_tmuxMenu->addAction(themeIcon({"list-add"}), tr("&New Session…"),
                          this, &MainWindow::newTmuxSession);
    m_tmuxMenu->addAction(themeIcon({"network-connect", "network-server"}),
                          tr("&Connect to Host…"), this, &MainWindow::connectTmuxHost);

    m_tmuxMenu->addSeparator();
    TmuxSession *current = tmuxSessionFor(currentTab());
    QAction *detach = m_tmuxMenu->addAction(
        current ? tr("&Detach from “%1”").arg(current->label()) : tr("&Detach"));
    detach->setEnabled(current != nullptr);
    detach->setToolTip(tr("Close its tabs here; tmux keeps the session running"));
    connect(detach, &QAction::triggered, this, [this]() {
        if (auto *s = tmuxSessionFor(currentTab()))
            s->detach();
    });
}

void MainWindow::newTmuxSession()
{
    // Suggest a name tmux doesn't have yet.
    QStringList taken;
    {
        QProcess list;
        const QStringList program = tmuxProgram();
        list.start(program.first(), program.mid(1) + QStringList{
            QStringLiteral("list-sessions"), QStringLiteral("-F"), QStringLiteral("#{session_name}")});
        if (list.waitForFinished(2000))
            taken = QString::fromUtf8(list.readAllStandardOutput()).split(QLatin1Char('\n'));
    }
    QString suggestion = QStringLiteral("main");
    for (int n = 2; taken.contains(suggestion); ++n)
        suggestion = QStringLiteral("main%1").arg(n);

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New tmux Session"), tr("Session name:"),
                                               QLineEdit::Normal, suggestion, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    // -A: if it exists after all, attach to it.
    attachTmux(name, tmuxProgram() + QStringList{
        QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("new-session"),
        QStringLiteral("-A"), QStringLiteral("-s"), name});
}

void MainWindow::connectTmuxHost()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Connect to tmux on a Host"));
    auto *form = new QFormLayout(&dialog);
    auto *host = new QLineEdit(&dialog);
    host->setPlaceholderText(tr("user@host"));
    auto *session = new QLineEdit(QStringLiteral("main"), &dialog);
    form->addRow(tr("Host"), host);
    form->addRow(tr("Session"), session);
    auto *hint = new QLabel(tr("Connects with ssh, which must log in without a "
                               "password prompt (a key or an agent). The session "
                               "is created if it doesn't exist."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString h = host->text().trimmed();
    const QString name = session->text().trimmed().isEmpty() ? QStringLiteral("main")
                                                              : session->text().trimmed();
    if (h.isEmpty())
        return;
    // -T: no remote terminal; control mode speaks over plain pipes. BatchMode
    // makes ssh fail rather than wait for a password nobody can type.
    attachTmux(QStringLiteral("%1: %2").arg(h, name), {
        QStringLiteral("ssh"), QStringLiteral("-T"), QStringLiteral("-o"),
        QStringLiteral("BatchMode=yes"), h,
        QStringLiteral("tmux -u -C new-session -A -s ") + shellQuote(name)});
}

void MainWindow::attachTmux(const QString &label, const QStringList &command)
{
    for (TmuxSession *s : std::as_const(m_tmuxSessions)) {
        if (s->label() != label)
            continue;
        qCInfo(lcTmux) << "attach" << label << "already open with" << s->tabs().size() << "tabs";
        if (!s->tabs().isEmpty()) {
            m_tabs->setCurrentWidget(s->tabs().first());
            return;
        }
        // Still connected but showing nothing, so attaching again would
        // otherwise do nothing at all: drop it (detaching) and start afresh.
        m_tmuxSessions.removeOne(s);
        disconnect(s, nullptr, this, nullptr);
        s->deleteLater();
        break;
    }

    auto *session = new TmuxSession(label, command, m_settings,
                                    m_profiles.defaultProfile(), this);
    m_tmuxSessions << session;
    auto shownAny = std::make_shared<bool>(false);

    connect(session, &TmuxSession::tabAdded, this,
            [this, session, shownAny](TerminalTab *tab, const QString &name) {
        const int index = addTab(tab, name, tr("tmux %1 — %2").arg(session->label(), name));
        if (!*shownAny) {
            *shownAny = true;
            m_tabs->setCurrentIndex(index);
            rebuildTmuxMenu();   // for a global menu, which may not ask again
        }
    });
    connect(session, &TmuxSession::tabRenamed, this, [this](TerminalTab *tab, const QString &) {
        syncTabFromActivePane(tab);
    });
    connect(session, &TmuxSession::tabActivated, this, [this](TerminalTab *tab) {
        if (m_tabs->indexOf(tab) >= 0)
            m_tabs->setCurrentWidget(tab);
    });
    connect(session, &TmuxSession::tabRemoved, this, &MainWindow::removeTab);
    connect(session, &TmuxSession::ended, this,
            [this, session, shownAny](const QString &reason) {
        qCInfo(lcTmux) << "session" << session->label() << "ended:" << reason;
        m_tmuxSessions.removeOne(session);
        session->deleteLater();
        rebuildTmuxMenu();
        if (!*shownAny) {
            QMessageBox::warning(this, tr("tmux"),
                                 tr("Could not attach to tmux session “%1”.").arg(session->label())
                                     + (reason.isEmpty() ? QString() : QStringLiteral("\n\n") + reason));
            return;
        }
        // tmux says "detached" for most endings, so whether we asked is what
        // tells a detach from the session (or the connection) going away.
        QString message;
        if (session->detachRequested())
            message = tr("Detached from tmux session “%1”").arg(session->label());
        else if (reason.isEmpty() || reason == QLatin1String("detached"))
            message = tr("tmux session “%1” ended").arg(session->label());
        else
            message = tr("tmux session “%1” ended: %2").arg(session->label(), reason);
        statusBar()->showMessage(message, 6000);
    });

    // The size tabs will have, so the session starts at the right size rather
    // than redrawing once its first tab is shown.
    if (QWidget *page = m_tabs->currentWidget()) {
        const int m = m_settings.appearance().terminalMargin;
        session->setClientArea(page->size() - QSize(2 * m, 2 * m));
    }
    session->start();
}

void MainWindow::newTabWithProfile(const QString &profileId)
{
    // An empty or dangling id means "the default profile". profileById()
    // already falls back, so this cannot fail to find something to open with.
    const TerminalProfile profile =
        profileId.isEmpty() ? m_profiles.defaultProfile()
                            : m_profiles.profileById(profileId);

    auto *term = makePane(profile);
    auto *tab = new TerminalTab(term);

    // The tab tooltip carries the profile, which is the only cue in the
    // standard layout that a tab is not just "another terminal".
    const int index = addTab(tab, tr("Terminal"),
                             tr("%1 — %2").arg(profile.name, tr("Terminal")));
    m_tabs->setCurrentIndex(index);
    term->focusTerminal();
}

void MainWindow::closeTab(int index)
{
    auto *tab = tabAt(index);
    if (auto *session = tmuxSessionFor(tab)) {
        if (!confirmTmuxClose(session, tab))
            return;
        session->killWindow(tab);  // the tab goes when tmux says the window has
        return;
    }
    if (tab)
        removeTab(tab);
}

bool MainWindow::confirmTmuxClose(TmuxSession *session, TerminalTab *tab)
{
    // Closing a session's last window ends the session, as in tmux, which is
    // rarely what closing a tab here means: offer to detach instead.
    // Closing a pane only matters when it is that window's last one.
    if (session->tabs().size() != 1 || !tab || tab->panes().size() != 1)
        return true;

    QMessageBox box(QMessageBox::Question, tr("Close tmux Session?"),
                    tr("“%1” is the last window of tmux session “%2”. Closing it "
                       "ends the session.")
                        .arg(session->windowName(tab), session->label()),
                    QMessageBox::NoButton, this);
    box.setInformativeText(tr("Detach to keep the session running, so you can attach "
                              "to it again from the tmux menu."));
    QPushButton *detach = box.addButton(tr("&Detach"), QMessageBox::AcceptRole);
    QPushButton *end = box.addButton(tr("&End Session"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(detach);
    box.exec();
    qCInfo(lcTmux) << "closing last window of" << session->label() << "->"
                   << (box.clickedButton() ? box.clickedButton()->text() : QString());

    if (box.clickedButton() == detach)
        session->detach();
    return box.clickedButton() == end;
}

void MainWindow::applyAppearanceToAll()
{
    for (TerminalWidget *t : allPanes())
        t->applyAppearance();
    updateTabLayout();
}

void MainWindow::updateTabLayout()
{
    const bool thumbs =
        m_settings.appearance().tabLayout == QStringLiteral("thumbnails");

    m_sidebar->setVisible(thumbs);
    // In thumbnail mode the cards carry the titles, so the tab bar is noise.
    m_tabs->tabBar()->setVisible(!thumbs);

    if (thumbs) {
        // Back from being dragged shut (or never sized): give it its width.
        if (m_splitter->sizes().value(0) < m_sidebar->minimumWidth()) {
            const int saved = m_settings.sidebarWidth();
            setSidebarWidth(saved >= m_sidebar->minimumWidth() ? saved : kDefaultSidebarWidth);
        }
        updateSidebarColours();
        syncTabTitles();
    }
}

void MainWindow::syncTabTitles()
{
    QStringList titles;
    titles.reserve(m_tabs->count());
    for (int i = 0; i < m_tabs->count(); ++i)
        titles << m_tabs->tabText(i);
    m_sidebar->syncTabs(titles, m_tabs->currentIndex());
    refreshPreviews(true);
}

void MainWindow::refreshPreviews(bool all)
{
    if (m_settings.appearance().tabLayout != QStringLiteral("thumbnails"))
        return;
    const QSize size = m_sidebar->previewSize();
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = tabAt(i);
        if (!tab || (!all && !tab->hasNewOutput()))
            continue;
        tab->clearNewOutput();
        m_sidebar->setPreview(i, tab->previewPixmap(size, dpr));
        updateCardDetails(i);
    }
}

void MainWindow::updateCardDetails(int index)
{
    auto *tab = tabAt(index);
    TerminalWidget *pane = tab ? tab->activePane() : nullptr;
    if (!pane)
        return;

    // tmux cards say which session they belong to; the second line is the
    // active pane's title when that says more than the window's name.
    if (auto *session = tmuxSessionFor(tab)) {
        m_sidebar->setBadge(index, tr("tmux %1").arg(session->label()));
        const QString title = pane->sessionTitle();
        m_sidebar->setSubtitle(index, title == session->windowName(tab) ? QString() : title);
        return;
    }

    // Local: what's running (when it isn't just the shell) and where.
    m_sidebar->setBadge(index, QString());
    QStringList parts;
    if (pane->hasRunningProcess()) {
        const QString program = pane->foregroundProgramName();
        if (!program.isEmpty())
            parts << program;
    }
    const QString dir = pane->currentDirectory();
    if (!dir.isEmpty())
        parts << tildePath(dir);
    if (tab->panes().size() > 1)
        parts << tr("%n panes", nullptr, int(tab->panes().size()));
    m_sidebar->setSubtitle(index, parts.join(QStringLiteral("  ·  ")));
}

void MainWindow::updateSidebarColours()
{
    if (auto *tab = currentTab())
        m_sidebar->setColours(tab->backgroundColour(), tab->foregroundColour());
    m_splitter->handle(1)->update();
}

void MainWindow::showTabMenu(int index, const QPoint &globalPos)
{
    QMenu menu(this);
    if (index >= 0) {
        menu.addAction(tr("&Rename…"), this, [this, index]() { renameTab(index); });
        menu.addSeparator();
        menu.addAction(themeIcon({"tab-close", "window-close"}), tr("&Close Tab"), this,
                       [this, index]() { closeTab(index); });
        QAction *others = menu.addAction(tr("Close &Other Tabs"), this, [this, index]() {
            QWidget *keep = m_tabs->widget(index);
            for (int i = m_tabs->count() - 1; i >= 0; --i) {
                if (m_tabs->widget(i) != keep)
                    closeTab(i);
            }
        });
        others->setEnabled(m_tabs->count() > 1);
        menu.addSeparator();
    }
    menu.addAction(themeIcon({"tab-new", "list-add"}), tr("&New Tab"), this, &MainWindow::newTab);
    menu.exec(globalPos);
}

void MainWindow::renameTab(int index)
{
    auto *tab = tabAt(index);
    if (!tab)
        return;
    auto *session = tmuxSessionFor(tab);
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Tab"),
        session ? tr("tmux window name:")
                : tr("Tab name (leave empty to follow the terminal's title):"),
        QLineEdit::Normal,
        session ? session->windowName(tab)
                : (tab->customTitle().isEmpty() ? m_tabs->tabText(index) : tab->customTitle()),
        &ok).trimmed();
    if (!ok)
        return;
    if (session) {
        // tmux renames it and says so; the tab follows from that.
        if (!name.isEmpty())
            session->renameWindow(tab, name);
        return;
    }
    tab->setCustomTitle(name);
    syncTabFromActivePane(tab);
}

void MainWindow::updateStatus()
{
    auto *term = currentTerminal();
    if (!term) {
        m_statusDir->clear();
        m_statusInfo->clear();
        return;
    }
    const QString title = term->sessionTitle();
    if (auto *session = tmuxSessionFor(term))
        m_statusDir->setText(tr("tmux %1  ·  %2").arg(
            session->label(), title.isEmpty() ? session->windowName(tabOf(term)) : title));
    else
        m_statusDir->setText(title);

    auto *t = term->terminal();
    // Report the tab's *effective* scheme, which a profile may have overridden.
    m_statusInfo->setText(tr("%1 × %2   ·   scrollback %3   ·   %4")
                              .arg(t->screenColumnsCount())
                              .arg(t->screenLinesCount())
                              .arg(t->historySize())
                              .arg(term->appliedColorScheme()));
}

void MainWindow::zoomBy(int halfPoints)
{
    const double next = m_settings.appearance().fontSize + (halfPoints / 2.0);
    m_settings.setFontSize(qBound(8.0, next, 32.0));
}

bool MainWindow::confirmQuitIfBusy()
{
    if (!m_settings.scrolling().confirmCloseWithRunningProcesses)
        return true;

    // Collect the terminals that would lose work. Tab order, so the list
    // matches what the user sees in the tab strip / sidebar.
    // Every pane counts, not just each tab's active one.
    struct Busy { TerminalWidget *pane; QString program; };
    QVector<Busy> busy;
    for (TerminalWidget *t : allPanes()) {
        if (t->hasRunningProcess())
            busy.append({ t, t->foregroundProgramName() });
    }
    if (busy.isEmpty())
        return true;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Quit SuperTerm?"));

    // Name the programs where possible: "Quitting will terminate micro, ssh"
    // is far more useful than a bare count.
    QStringList names;
    for (const Busy &b : busy) {
        QString label = b.program.isEmpty() ? tr("a program")
                                            : b.program;
        const QString title = b.pane->sessionTitle().trimmed();
        if (!title.isEmpty())
            label += QStringLiteral(" (%1)").arg(title);
        names << label;
    }

    if (busy.size() == 1) {
        box.setText(tr("A program is still running in one terminal."));
        box.setInformativeText(
            tr("Quitting will terminate %1. Any unsaved work will be lost.")
                .arg(names.first()));
    } else {
        box.setText(tr("Programs are still running in %1 terminals.")
                        .arg(busy.size()));
        box.setInformativeText(
            tr("Quitting will terminate:\n\n%1\n\nAny unsaved work will be lost.")
                .arg(names.join(QLatin1Char('\n'))));
    }

    QPushButton *quitBtn = box.addButton(tr("&Quit Anyway"),
                                         QMessageBox::DestructiveRole);
    QPushButton *cancelBtn = box.addButton(tr("&Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancelBtn);   // safe default: Enter cancels
    box.setEscapeButton(cancelBtn);
    box.exec();

    return box.clickedButton() == quitBtn;
}

void MainWindow::refreshProfiles()
{
    // Rebuild the submenu. Profiles are few, so clearing and refilling is
    // simpler than diffing, and it keeps ordering identical to the store.
    if (m_newTabWithProfileMenu) {
        m_newTabWithProfileMenu->clear();
        const auto &all = m_profiles.profiles();
        if (all.isEmpty()) {
            m_newTabWithProfileMenu->setEnabled(false);
        } else {
            m_newTabWithProfileMenu->setEnabled(true);
            for (const auto &p : all) {
                const QString id = p.id;
                QAction *act = m_newTabWithProfileMenu->addAction(p.name);
                if (p.isDefault)
                    act->setToolTip(tr("The default profile — what New Tab opens"));
                connect(act, &QAction::triggered, this,
                        [this, id]() { newTabWithProfile(id); });
            }
        }
    }

    /*
     * Push updated definitions onto open tabs.
     *
     * Only the appearance is re-applied (setProfile): the shell stays running,
     * because restarting it to pick up a new shell program would destroy
     * whatever is in the tab. A tab whose profile was deleted keeps the
     * settings it resolved with, which is the least surprising outcome.
     */
    for (TerminalWidget *t : allPanes()) {
        if (t->isExternal())
            continue;   // tmux panes follow their session, not a profile
        const TerminalProfile current = m_profiles.profileById(t->profileId());
        // profileById() falls back to the default for a dangling id; do not
        // silently reassign a deleted tab to the default profile.
        if (current.id != t->profileId())
            continue;
        t->setProfile(current);
    }
}


void MainWindow::openSettings(int section)
{
    if (!m_settingsDialog)
        m_settingsDialog = new SettingsDialog(m_settings, m_profiles, this);
    m_settingsDialog->showSection(static_cast<SettingsDialog::Section>(section));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Refuse to take running work down with us without asking.
    if (!confirmQuitIfBusy()) {
        event->ignore();
        return;
    }
    // Drop any in-place-edit stand-in and put the real scheme name back, so the
    // stored config never points at a throwaway file.
    m_settings.finaliseLiveSchemes();
    m_settings.saveWindowGeometry(saveGeometry(), saveState());
    QMainWindow::closeEvent(event);
}
