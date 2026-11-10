/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "settingsdialog.h"
#include "terminalwidget.h"
#include "thumbnailsidebar.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <Emulation.h>
#include <qtermwidget.h>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SuperTerm"));
    setMinimumSize(760, 420);

    buildLayout();
    buildMenus();
    buildStatusBar();

    // Live previews. Only the visible terminal can be grabbed meaningfully, so
    // this refreshes the current tab and leaves the others on their last image.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(2500);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::refreshThumbnail);
    m_previewTimer->start();

    connect(&m_settings, &Settings::appearanceChanged,
            this, &MainWindow::applyAppearanceToAll);
    connect(&m_settings, &Settings::scrollingChanged, this, [this]() {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *t = terminalAt(i))
                t->applyScrolling();
        }
    });

    // Restore geometry from the previous session.
    if (!m_settings.windowGeometry().isEmpty())
        restoreGeometry(m_settings.windowGeometry());
    else
        resize(1100, 650);
    if (!m_settings.windowState().isEmpty())
        restoreState(m_settings.windowState());

    newTab();
    updateTabLayout();
}

void MainWindow::buildLayout()
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    m_sidebar = new ThumbnailSidebar(m_splitter);

    m_tabs = new QTabWidget(m_splitter);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);

    // Chrome buttons for the top-tab layout.
    //
    // In thumbnail mode the sidebar header carries these, and the tab bar is
    // hidden, so a corner widget on the tab widget is exactly the right place:
    // it is only visible when tabs are visible.
    auto *corner = new QWidget(m_tabs);
    auto *cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 4, 0);
    cornerLayout->setSpacing(0);

    auto makeCornerButton = [corner](const QString &text, const QString &tip) {
        auto *b = new QToolButton(corner);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        // Never steal keyboard focus from the terminal.
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };

    auto *newBtn = makeCornerButton(QStringLiteral("＋"), tr("New tab (Ctrl+Shift+T)"));
    auto *sidebarBtn = makeCornerButton(QStringLiteral("▤"), tr("Use thumbnail sidebar"));
    auto *settingsBtn = makeCornerButton(QStringLiteral("⚙"), tr("Settings (Ctrl+,)"));
    cornerLayout->addWidget(newBtn);
    cornerLayout->addWidget(sidebarBtn);
    cornerLayout->addWidget(settingsBtn);
    m_tabs->setCornerWidget(corner, Qt::TopRightCorner);

    connect(newBtn, &QToolButton::clicked, this, &MainWindow::newTab);
    connect(settingsBtn, &QToolButton::clicked, this, [this]() { openSettings(); });
    connect(sidebarBtn, &QToolButton::clicked, this, [this]() {
        m_settings.setTabLayout(QStringLiteral("thumbnails"));
    });

    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_tabs);
    setCentralWidget(m_splitter);

    // Pin the splitter to the sidebar's own width.
    //
    // A QSplitter keeps its remembered handle position and hands the sidebar
    // that width even when the sidebar wants less, leaving a dead strip before
    // the terminal (that strip is why dragging the handle appeared to "fix"
    // it). Sizes must be set after both children are in place.
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({m_sidebar->sizeHint().width(), 1});

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        m_sidebar->setCurrentIndex(index);
        updateStatus();
        refreshThumbnail();
    });

    connect(m_sidebar, &ThumbnailSidebar::tabActivated, this, [this](int index) {
        if (index >= 0 && index < m_tabs->count() && index != m_tabs->currentIndex())
            m_tabs->setCurrentIndex(index);
    });
    connect(m_sidebar, &ThumbnailSidebar::tabClosed, this, &MainWindow::closeTab);
    connect(m_sidebar, &ThumbnailSidebar::addTabRequested, this, &MainWindow::newTab);
    connect(m_sidebar, &ThumbnailSidebar::settingsRequested, this, [this]() { openSettings(); });
    connect(m_sidebar, &ThumbnailSidebar::layoutToggleRequested, this, [this]() {
        m_settings.setTabLayout(QStringLiteral("standard"));
    });
}

void MainWindow::buildMenus()
{
    auto *file = menuBar()->addMenu(tr("&File"));

    auto *newAct = file->addAction(tr("&New Tab"));
    newAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    connect(newAct, &QAction::triggered, this, &MainWindow::newTab);

    auto *closeAct = file->addAction(tr("&Close Tab"));
    closeAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(closeAct, &QAction::triggered, this, [this]() { closeTab(m_tabs->currentIndex()); });

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
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, qApp, &QApplication::quit);

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

    auto *view = menuBar()->addMenu(tr("&View"));

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
    auto *toggleLayout = view->addAction(tr("&Thumbnail Sidebar"));
    toggleLayout->setCheckable(true);
    toggleLayout->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    connect(toggleLayout, &QAction::triggered, this, [this](bool on) {
        m_settings.setTabLayout(on ? QStringLiteral("thumbnails")
                                   : QStringLiteral("standard"));
    });
    // Keep the check state in step with the setting, however it changed.
    connect(&m_settings, &Settings::appearanceChanged, this, [this, toggleLayout]() {
        toggleLayout->setChecked(
            m_settings.appearance().tabLayout == QStringLiteral("thumbnails"));
    });
}

void MainWindow::buildStatusBar()
{
    m_statusDir = new QLabel(this);
    m_statusInfo = new QLabel(this);
    statusBar()->addWidget(m_statusDir, 1);
    statusBar()->addPermanentWidget(m_statusInfo);
}

TerminalWidget *MainWindow::currentTerminal() const
{
    return qobject_cast<TerminalWidget *>(m_tabs->currentWidget());
}

TerminalWidget *MainWindow::terminalAt(int index) const
{
    if (index < 0 || index >= m_tabs->count())
        return nullptr;
    return qobject_cast<TerminalWidget *>(m_tabs->widget(index));
}

void MainWindow::newTab()
{
    auto *term = new TerminalWidget(m_nextTabId++, m_settings, this);
    term->applyAppearance();
    term->applyScrolling();

    const int index = m_tabs->addTab(term, tr("Terminal"));
    m_tabs->setCurrentIndex(index);
    term->focusTerminal();

    connect(term, &TerminalWidget::titleChanged, this, [this, term](const QString &title) {
        const int i = m_tabs->indexOf(term);
        if (i < 0)
            return;
        m_tabs->setTabText(i, title);
        m_tabs->setTabToolTip(i, title);
        m_sidebar->setTitle(i, title);
        if (term == currentTerminal())
            setWindowTitle(tr("%1 — SuperTerm").arg(title));
    });

    connect(term, &TerminalWidget::sessionFinished, this, [this, term]() {
        const int i = m_tabs->indexOf(term);
        if (i >= 0)
            closeTab(i);
    });

    syncTabTitles();
    refreshThumbnail();
}

void MainWindow::closeTab(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;

    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    w->deleteLater();

    if (m_tabs->count() == 0) {
        newTab();
        return;
    }
    syncTabTitles();
    updateStatus();
}

void MainWindow::applyAppearanceToAll()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = terminalAt(i))
            t->applyAppearance();
    }
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
        syncTabTitles();
        refreshThumbnail();
    }
}

void MainWindow::syncTabTitles()
{
    QStringList titles;
    titles.reserve(m_tabs->count());
    for (int i = 0; i < m_tabs->count(); ++i)
        titles << m_tabs->tabText(i);
    m_sidebar->syncTabs(titles, m_tabs->currentIndex());
}

void MainWindow::refreshThumbnail()
{
    if (m_settings.appearance().tabLayout != QStringLiteral("thumbnails"))
        return; // nothing to show

    auto *term = currentTerminal();
    if (!term)
        return;

    const QPixmap pm = term->previewPixmap(QSize(150, 90));
    if (!pm.isNull())
        m_sidebar->setPreview(m_tabs->currentIndex(), pm);
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
    m_statusDir->setText(title);

    auto *t = term->terminal();
    m_statusInfo->setText(tr("%1 × %2   ·   scrollback %3   ·   %4")
                              .arg(t->screenColumnsCount())
                              .arg(t->screenLinesCount())
                              .arg(t->historySize())
                              .arg(m_settings.appearance().colorScheme));
}

void MainWindow::zoomBy(int halfPoints)
{
    const double next = m_settings.appearance().fontSize + (halfPoints / 2.0);
    m_settings.setFontSize(qBound(8.0, next, 32.0));
}

void MainWindow::openSettings(int section)
{
    if (!m_settingsDialog)
        m_settingsDialog = new SettingsDialog(m_settings, this);
    m_settingsDialog->showSection(static_cast<SettingsDialog::Section>(section));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_settings.saveWindowGeometry(saveGeometry(), saveState());
    QMainWindow::closeEvent(event);
}
