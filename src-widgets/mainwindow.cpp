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
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
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
 * The first of several freedesktop icon names that the current theme has.
 * Themes disagree on names (Breeze and Adwaita each lack some of the others'),
 * so each toolbar action lists a few. If none exists the icon is null and the
 * toolbar button shows the action's text instead.
 */
QIcon themeIcon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QString n = QString::fromLatin1(name);
        if (QIcon::hasThemeIcon(n))
            return QIcon::fromTheme(n);
    }
    return QIcon();
}

/*
 * Toolbar buttons are icon-only, so the tooltip is the only place a button's
 * name and shortcut appear. Qt doesn't add the shortcut itself.
 */
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

    // Live previews. Only the visible terminal can be grabbed meaningfully, so
    // this refreshes the current tab and leaves the others on their last image.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(2500);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::captureNextPreview);
    m_previewTimer->start();

    connect(&m_settings, &Settings::appearanceChanged,
            this, &MainWindow::applyAppearanceToAll);
    connect(&m_settings, &Settings::scrollingChanged, this, [this]() {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (auto *t = terminalAt(i))
                t->applyScrolling();
        }
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
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    m_sidebar = new ThumbnailSidebar(m_splitter);

    m_tabs = new QTabWidget(m_splitter);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->tabBar()->setExpanding(false);

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
        // Selecting a tab counts as having seen it, so clear its "new output"
        // marker (and the underlying dirty flag) straight away rather than
        // waiting for the next preview capture.
        if (auto *term = terminalAt(index)) {
            term->clearNewOutput();
            m_sidebar->setBusy(index, false);
        }
        updateStatus();
        captureNextPreview();
    });

    connect(m_sidebar, &ThumbnailSidebar::tabActivated, this, [this](int index) {
        if (index >= 0 && index < m_tabs->count() && index != m_tabs->currentIndex())
            m_tabs->setCurrentIndex(index);
    });
    connect(m_sidebar, &ThumbnailSidebar::tabClosed, this, &MainWindow::closeTab);
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
    copyAct->setIcon(themeIcon({"edit-copy"}));
    pasteAct->setIcon(themeIcon({"edit-paste"}));
    findAct->setIcon(themeIcon({"edit-find", "system-search"}));
    zoomOut->setIcon(themeIcon({"zoom-out"}));
    zoomIn->setIcon(themeIcon({"zoom-in"}));
    toggleLayout->setIcon(themeIcon({"sidebar-show", "sidebar-show-symbolic", "view-preview"}));
    settingsAct->setIcon(themeIcon({"configure", "preferences-system", "document-properties"}));
    for (QAction *act : {newAct, closeAct, copyAct, pasteAct, findAct, zoomOut, zoomIn,
                         zoomReset, toggleLayout, settingsAct})
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
    bar->addAction(copyAct);
    bar->addAction(pasteAct);
    bar->addAction(findAct);

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
    newTabWithProfile(QString());
}

void MainWindow::newTabWithProfile(const QString &profileId)
{
    // An empty or dangling id means "the default profile". profileById()
    // already falls back, so this cannot fail to find something to open with.
    const TerminalProfile profile =
        profileId.isEmpty() ? m_profiles.defaultProfile()
                            : m_profiles.profileById(profileId);

    auto *term = new TerminalWidget(m_nextTabId++, m_settings, profile, this);
    term->applyAppearance();
    term->applyScrolling();

    const int index = m_tabs->addTab(term, tr("Terminal"));
    // The tab tooltip carries the profile, which is the only cue in the
    // standard layout that a tab is not just "another terminal".
    m_tabs->setTabToolTip(index, tr("%1 — %2").arg(profile.name, tr("Terminal")));
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

    // Mark a tab as having new output when it is not the one on screen. This is
    // the visible "something changed here" cue, and it also tells the preview
    // rotation which tab is worth re-capturing.
    connect(term, &TerminalWidget::outputArrived, this, [this, term]() {
        const int i = m_tabs->indexOf(term);
        if (i < 0)
            return;
        if (i != m_tabs->currentIndex())
            m_sidebar->setBusy(i, true);
    });

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
        const int i = m_tabs->indexOf(term);
        if (i >= 0 && i != m_tabs->currentIndex())
            m_sidebar->setBusy(i, true);
    };
    connect(term, &TerminalWidget::activityDetected, this, markIfBackground);
    connect(term, &TerminalWidget::silenceDetected,  this, markIfBackground);

    syncTabTitles();
    captureNextPreview();
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
        captureNextPreview();
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

/*
 * Capture a preview for a tab whose output has changed.
 *
 * Two measured facts drive this:
 *
 * 1. A QTabWidget page that is not current never paints, and an unpainted
 *    widget grabs as a blank pixmap (a hidden page also reports 100x30 until
 *    laid out). Neither grab() nor render() helps; switching is required.
 * 2. receivedData() fires from the pty even for hidden tabs, so we know which
 *    tab actually changed without looking at it.
 *
 * Together: the rotation only visits tabs that have produced output since their
 * last capture. An idle set of tabs costs nothing, and the visible tab stays
 * put unless something changed.
 *
 * A capture spans two ticks -- show the tab, then grab it -- because a
 * same-tick grab yields a blank image; the paint has not happened yet.
 */
void MainWindow::captureNextPreview()
{
    if (m_settings.appearance().tabLayout != QStringLiteral("thumbnails"))
        return;
    if (m_tabs->count() == 0)
        return;

    // Second half of a capture: the target has now had a chance to paint.
    if (m_capturingPreview) {
        if (auto *term = terminalAt(m_previewTarget)) {
            const QPixmap pm = term->previewPixmap(QSize(150, 90));
            if (!pm.isNull())
                m_sidebar->setPreview(m_previewTarget, pm);
            term->clearNewOutput();
        }
        if (m_restoreTab >= 0 && m_restoreTab < m_tabs->count())
            m_tabs->setCurrentIndex(m_restoreTab);
        m_capturingPreview = false;
        m_previewTarget = -1;
        m_restoreTab = -1;
        return;
    }

    // Prefer a tab whose output changed since its last capture.
    int target = -1;
    for (int offset = 0; offset < m_tabs->count(); ++offset) {
        const int i = (m_nextPreviewTab + offset) % m_tabs->count();
        auto *t = terminalAt(i);
        if (t && t->hasNewOutput()) {
            target = i;
            break;
        }
    }

    if (target < 0) {
        // Nothing dirty: nothing worth switching to. Re-grab the visible tab
        // only, since its image can be stale after a resize or a theme change.
        if (auto *term = currentTerminal()) {
            const QPixmap pm = term->previewPixmap(QSize(150, 90));
            if (!pm.isNull())
                m_sidebar->setPreview(m_tabs->currentIndex(), pm);
            term->clearNewOutput();
        }
        return;
    }

    m_nextPreviewTab = (target + 1) % m_tabs->count();
    m_previewTarget = target;

    if (m_previewTarget == m_tabs->currentIndex()) {
        // Already visible; grab straight away without switching.
        if (auto *term = terminalAt(m_previewTarget)) {
            const QPixmap pm = term->previewPixmap(QSize(150, 90));
            if (!pm.isNull())
                m_sidebar->setPreview(m_previewTarget, pm);
            term->clearNewOutput();
        }
        m_previewTarget = -1;
        return;
    }

    m_restoreTab = m_tabs->currentIndex();
    m_capturingPreview = true;
    m_tabs->setCurrentIndex(m_previewTarget);
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
    struct Busy { int index; QString program; };
    QVector<Busy> busy;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *t = terminalAt(i)) {
            if (t->hasRunningProcess())
                busy.append({ i, t->foregroundProgramName() });
        }
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
        auto *t = terminalAt(b.index);
        const QString title = t ? t->sessionTitle().trimmed() : QString();
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
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *t = terminalAt(i);
        if (!t)
            continue;
        const TerminalProfile current = m_profiles.profileById(t->profileId());
        // profileById() falls back to the default for a dangling id; do not
        // silently reassign a deleted tab to the default profile.
        if (current.id != t->profileId())
            continue;
        t->setProfile(current);
        // Keep the tooltip's profile name current after a rename.
        if (!t->sessionTitle().isEmpty())
            m_tabs->setTabToolTip(i, tr("%1 — %2").arg(current.name,
                                                       t->sessionTitle()));
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
