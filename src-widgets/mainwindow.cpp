/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "appicon.h"
#include "formername.h"
#include "macwindow.h"
#include "notifier.h"
#include "procinfo.h"
#include "programstatus.h"
#include "settingsdialog.h"
#include "tabstrip.h"
#include "terminaltab.h"
#include "terminalwidget.h"
#include "thumbnailsidebar.h"
#include "tmuxcontrol.h"
#include "tmuxsession.h"
#include "tmuxworkspace.h"
#include "vncclient.h"
#ifdef PUDDLEJUMPER_RDP
#include "rdpclient.h"
#endif
#include "vnctab.h"
#include "vncview.h"
#ifdef PUDDLEJUMPER_WEB
#include "webtab.h"
#endif
#include "wsl.h"
#ifdef PUDDLEJUMPER_REX
#include "rexsession.h"
#endif

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QResizeEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QProcess>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QSplitter>
#include <QSettings>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <Emulation.h>
#include <qtermwidget.h>

namespace {

/*
 * An action's name for Rex keymaps, palettes and client.perform (rexia's
 * name where rexia has the same action), and its category.
 */
void tagRexAction(QAction *action, const char *name, const char *category)
{
    action->setProperty("rexAction", QString::fromLatin1(name));
    action->setProperty("rexCategory", QString::fromLatin1(category));
}

/*
 * Toolbar buttons are icon-only, so the tooltip is the only place a button's
 * name and shortcut appear. Qt doesn't add the shortcut itself.
 */
/*
 * How to run tmux. PUDDLEJUMPER_TMUX overrides it, e.g. "tmux -L test" to use a
 * separate server (handy for testing without touching real sessions).
 */
QStringList tmuxProgram()
{
    const QString env = appEnvString("TMUX").trimmed();
    const QStringList cmd = env.isEmpty() ? QStringList() : QProcess::splitCommand(env);
    if (!cmd.isEmpty())
        return cmd;
#ifdef Q_OS_WIN
    // Windows has no tmux of its own: the default WSL distribution's, if any
    // (unless one is on the path after all, as MSYS2's can be).
    if (QStandardPaths::findExecutable(QStringLiteral("tmux")).isEmpty()
        && !wsl::distributions().isEmpty())
        return {QStringLiteral("wsl.exe"), QStringLiteral("-e"), QStringLiteral("tmux")};
#endif
    return {QStringLiteral("tmux")};
}

/* Quote for a POSIX shell (the remote one, for ssh). */
QString shellQuote(const QString &arg)
{
    QString out = arg;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

/* The program and arguments that run @p script: a shell's -c. */
QStringList scriptCommand(const QString &script)
{
#ifdef Q_OS_WIN
    return {QStringLiteral("powershell.exe"), QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
            QStringLiteral("-Command"), script};
#else
    return {QStringLiteral("/bin/sh"), QStringLiteral("-c"), script};
#endif
}

constexpr int kDefaultSidebarWidth = 220;

// On a tab: it has output that hasn't been seen (kept while it is parked).
constexpr char kBusyProperty[] = "puddlejumperBusy";

// On a tab: the program status it was last notified of (see
// notifyProgramStatus()), when, and whether a look again is due.
constexpr char kNotified[] = "puddlejumperNotified";
constexpr char kNotifiedAt[] = "puddlejumperNotifiedAt";
constexpr char kPending[] = "puddlejumperNotifyPending";
constexpr qint64 kNotifyInterval = 5000;   // at most one a tab in this many ms

// On a tab (see floatBlockedTabs()): a program in it is waiting on you; it
// was moved to the front for that, and the tab it came after (null: it was
// first); it is to stay where it is until answered (it was current then, or
// moved by hand since).
constexpr char kBlocked[] = "puddlejumperBlocked";
constexpr char kFloated[] = "puddlejumperFloated";
constexpr char kFloatHome[] = "puddlejumperFloatHome";
constexpr char kFloatHeld[] = "puddlejumperFloatHeld";

// On a tab (see updateInbox()): what it last wanted of you, and since when
// (ms since the epoch).
constexpr char kInboxKey[] = "puddlejumperInboxKey";
constexpr char kInboxSince[] = "puddlejumperInboxSince";

// On a new tab: until when (ms since the epoch) its output is its shell
// starting up, not news.
constexpr char kQuietUntilProperty[] = "puddlejumperQuietUntil";
// On a pane: a command was running in it when last looked (see
// reloadAfterCommands()).
constexpr char kRunningProperty[] = "puddlejumperRunning";

// Where the session (workspaces and their tabs) is kept between runs.
constexpr auto kSessionKey = "session/state";

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
        // Against the terminals: their background, or with pane cards the
        // surface they sit on (the sidebar's own), so it reads as one piece.
        const QColor bg = TerminalWidget::paneCards() ? m_sidebar->palette().color(QPalette::Window)
                                                      : m_sidebar->background();
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
    setWindowTitle(QStringLiteral("PuddleJumper"));
    setMinimumSize(760, 420);

    // There is always a workspace; a saved session replaces this one.
    Workspace first;
    first.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    first.name = tr("Default");
    m_workspaces << first;

    buildLayout();
    buildMenus();
    buildStatusBar();
#ifdef PUDDLEJUMPER_REX
    // Keys in Rex panes go by the server's keymap (see routeRexKey).
    qApp->installEventFilter(this);
#endif

    // Live previews: a few times a second, redraw the cards of tabs that
    // have had output since. Previews are drawn from the panes' screens, so
    // hidden tabs update as well as the visible one, and idle tabs cost nothing.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(250);
    connect(m_previewTimer, &QTimer::timeout, this, [this]() {
        refreshPreviews(false);
        updateProjectNotice();   // cheap unless the shell has changed directory
        reloadAfterCommands();   // nothing unless a web tab asks for it
    });
    m_previewTimer->start();

    // Programs' notifications (see notifyProgramStatus()): clicking one shows
    // its tab; coming back to the window takes the current tab's away.
    m_notifier = new Notifier(this);
    connect(m_notifier, &Notifier::activated, this, [this](const QString &key) {
        for (Tab *page : allPages()) {
            if (notificationKey(page) == key) {
                showTab(page);
                break;
            }
        }
        if (isMinimized())
            showNormal();
        raise();
        activateWindow();
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && currentPage())
            m_notifier->withdraw(notificationKey(currentPage()));
    });

    connect(&m_settings, &Settings::appearanceChanged,
            this, &MainWindow::applyAppearanceToAll);
    connect(&m_settings, &Settings::scrollingChanged, this, [this]() {
        for (TerminalWidget *t : allPanes())
            t->applyScrolling();
        m_sidebar->setTintBlocked(m_settings.scrolling().tintBlockedTabs);
        m_tabStrip->setTintBlocked(m_settings.scrolling().tintBlockedTabs);
        scheduleFloatBlockedTabs();
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
    if (!m_settings.scrolling().restoreSession || !restoreSession())
        newTab();
    updateTabLayout();
    updateWorkspaceChrome();
}

void MainWindow::buildLayout()
{
    m_splitter = new SidebarSplitter(
        this, [this]() { return m_sidebar; },
        [this]() { setSidebarWidth(kDefaultSidebarWidth); });
    m_splitter->setHandleWidth(5);

    m_sidebar = new ThumbnailSidebar(m_splitter);

    m_parking = new QWidget(this);
    m_parking->hide();

    // The terminals, with the tab strip above them when the tabs are along
    // the top. The tab widget's own bar is never shown: the strip and the
    // sidebar stand in for it, drawn alike on every platform. It still owns
    // the pages and their order.
    auto *tabArea = new QWidget(m_splitter);
    auto *tabLayout = new QVBoxLayout(tabArea);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(0);
    m_tabStrip = new TabStrip(tabArea);
    m_tabs = new QTabWidget(tabArea);
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->hide();
    // Its size, the terminal area's, gives the previews their shape.
    m_tabs->installEventFilter(this);
    tabLayout->addWidget(m_tabStrip);
    tabLayout->addWidget(m_tabs, 1);

    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(tabArea);
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

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    // A tab dragged in the sidebar or the strip moves the page (below); the
    // other view follows here. The one it was dragged in is already in place.
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (m_moveSource != m_sidebar)
            m_sidebar->moveCard(from, to);
        if (m_moveSource != m_tabStrip)
            m_tabStrip->moveTab(from, to);
    });

    connect(m_sidebar, &ThumbnailSidebar::tabActivated, this, [this](int index) {
        if (index >= 0 && index < m_tabs->count() && index != m_tabs->currentIndex())
            m_tabs->setCurrentIndex(index);
    });
    connect(m_sidebar, &ThumbnailSidebar::tabClosed, this, &MainWindow::closeTab);
    connect(m_sidebar, &ThumbnailSidebar::tabMoved, this, [this](int from, int to) {
        m_moveSource = m_sidebar;
        m_tabs->tabBar()->moveTab(from, to);
        m_moveSource = nullptr;
        heldByHand(pageAt(to));
    });
    connect(m_sidebar, &ThumbnailSidebar::newTabRequested, this, &MainWindow::newTab);
    connect(m_sidebar, &ThumbnailSidebar::contextMenuRequested,
            this, &MainWindow::showTabMenu);
    connect(m_sidebar, &ThumbnailSidebar::previewSizeChanged,
            this, [this]() { refreshPreviews(true); });

    connect(m_tabStrip, &TabStrip::tabActivated, this, [this](int index) {
        if (index >= 0 && index < m_tabs->count() && index != m_tabs->currentIndex())
            m_tabs->setCurrentIndex(index);
    });
    connect(m_tabStrip, &TabStrip::tabClosed, this, &MainWindow::closeTab);
    connect(m_tabStrip, &TabStrip::tabMoved, this, [this](int from, int to) {
        m_moveSource = m_tabStrip;
        m_tabs->tabBar()->moveTab(from, to);
        m_moveSource = nullptr;
        heldByHand(pageAt(to));
    });
    connect(m_tabStrip, &TabStrip::newTabRequested, this, &MainWindow::newTab);
    connect(m_tabStrip, &TabStrip::contextMenuRequested, this, &MainWindow::showTabMenu);

    m_sidebar->setTintBlocked(m_settings.scrolling().tintBlockedTabs);
    m_tabStrip->setTintBlocked(m_settings.scrolling().tintBlockedTabs);
}

void MainWindow::onCurrentTabChanged(int index)
{
    // Mid-switch the tab widget fills and empties; the switch calls this once
    // it's done.
    if (m_switchingWorkspace)
        return;
    m_sidebar->setCurrentIndex(index);
    m_tabStrip->setCurrentIndex(index);
    if (auto *tab = pageAt(index)) {
        // Keep tmux's current window in step, so another client attaching
        // sees the same one.
        if (auto *session = tmuxSessionFor(tab))
            session->selectWindow(qobject_cast<TerminalTab *>(tab));
#ifdef PUDDLEJUMPER_REX
        if (auto *rex = rexSessionFor(tab))
            rex->windowShown(qobject_cast<TerminalTab *>(tab));
#endif
        // Selecting a tab counts as having seen its output, and what a
        // notification said about it.
        m_notifier->withdraw(notificationKey(tab));
        m_sidebar->setBusy(index, false);
        m_tabStrip->setBusy(index, false);
        tab->setProperty(kBusyProperty, false);
        tab->focusContent();
        syncTabText(tab);
    }
    updateTabColours();
    updateStatus();
}

void MainWindow::setSidebarWidth(int width)
{
    const int total = qMax(m_splitter->width(), width + 200);
    m_splitter->setSizes({width, total - width});
    m_settings.setSidebarWidth(width);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
#ifdef PUDDLEJUMPER_REX
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)
        && watched == QApplication::focusWidget() && routeRexKey(static_cast<QKeyEvent *>(event)))
        return true;
#endif
    if (watched == m_tabs && event->type() == QEvent::Resize) {
        // Cards take the terminal area's shape, which is the tab widget's: its
        // own bar is hidden. (The pages haven't been resized yet when this runs.)
        const QSize area = static_cast<QResizeEvent *>(event)->size();
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
    tagRexAction(newAct, "client.tab.new", "Tab");
    newAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    connect(newAct, &QAction::triggered, this, &MainWindow::newTab);

    // Populated by refreshProfiles(), which runs whenever the store changes.
    // A submenu rather than N top-level actions keeps the File menu stable as
    // profiles come and go.
    m_newTabWithProfileMenu = file->addMenu(tr("New Tab &with Profile"));

    // A remote screen in a tab of its own.
    auto *vncAct = file->addAction(tr("New &VNC Tab…"));
    connect(vncAct, &QAction::triggered, this, [this]() {
        QSettings settings;
        const QString last = settings.value(QStringLiteral("vnc/lastAddress"),
                                            QStringLiteral("localhost:5900")).toString();
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("New VNC Tab"),
                                                   tr("Address (host, host:port or host:display):"),
                                                   QLineEdit::Normal, last, &ok);
        if (!ok)
            return;
        settings.setValue(QStringLiteral("vnc/lastAddress"), text.trimmed());
        openVnc(text);
    });
#ifdef PUDDLEJUMPER_REX
    // A session on a Rex server.
    auto *rexAct = file->addAction(tr("New Re&x Tab…"));
    connect(rexAct, &QAction::triggered, this, &MainWindow::newRexTab);
#endif
#ifdef PUDDLEJUMPER_RDP
    auto *rdpAct = file->addAction(tr("New R&DP Tab…"));
    connect(rdpAct, &QAction::triggered, this, [this]() {
        QSettings settings;
        const QString last = settings.value(QStringLiteral("rdp/lastAddress")).toString();
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("New RDP Tab"),
                                                   tr("Address (host or host:port):"),
                                                   QLineEdit::Normal, last, &ok);
        if (!ok || text.trimmed().isEmpty())
            return;
        settings.setValue(QStringLiteral("rdp/lastAddress"), text.trimmed());
        openRdp(text);
    });
#endif
#ifdef PUDDLEJUMPER_WEB
    auto *webAct = file->addAction(tr("New W&eb Tab…"));
    connect(webAct, &QAction::triggered, this, [this]() {
        QSettings settings;
        const QString last = settings.value(QStringLiteral("web/lastAddress"),
                                            QStringLiteral("localhost:3000")).toString();
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("New Web Tab"), tr("Address:"),
                                                   QLineEdit::Normal, last, &ok);
        if (!ok || text.trimmed().isEmpty())
            return;
        const QUrl url = WebTab::urlFromInput(text);
        if (!url.isValid()) {
            QMessageBox::warning(this, tr("New Web Tab"), tr("“%1” isn't an address.").arg(text));
            return;
        }
        settings.setValue(QStringLiteral("web/lastAddress"), text.trimmed());
        openWeb(url);
    });
#endif

    auto *closeAct = file->addAction(tr("&Close Tab"));
    tagRexAction(closeAct, "client.tab.close", "Tab");
    closeAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    connect(closeAct, &QAction::triggered, this, [this]() { closeTab(m_tabs->currentIndex()); });

    auto *renameAct = file->addAction(tr("&Rename Tab…"));
    tagRexAction(renameAct, "client.tab.rename", "Tab");
    connect(renameAct, &QAction::triggered, this, [this]() { renameTab(m_tabs->currentIndex()); });

    auto *nextAct = file->addAction(tr("&Next Tab"));
    tagRexAction(nextAct, "client.tab.next", "Tab");
    nextAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")));
    connect(nextAct, &QAction::triggered, this, [this]() {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % qMax(1, m_tabs->count()));
    });

    auto *prevAct = file->addAction(tr("&Previous Tab"));
    tagRexAction(prevAct, "client.tab.previous", "Tab");
    prevAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
    connect(prevAct, &QAction::triggered, this, [this]() {
        const int n = qMax(1, m_tabs->count());
        m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + n) % n);
    });

    // Ctrl+Shift+J: unused by terminal programs, as the others here.
    m_nextNeedingYouAct = file->addAction(tr("Next Tab Needing &You"));
    tagRexAction(m_nextNeedingYouAct, "client.tab.next_needing_you", "Tab");
    m_nextNeedingYouAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+J")));
    connect(m_nextNeedingYouAct, &QAction::triggered, this, &MainWindow::showNextNeedingYou);

    file->addSeparator();
    auto *settingsAct = file->addAction(tr("&Settings…"));
    tagRexAction(settingsAct, "client.settings", "App");
    settingsAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAct, &QAction::triggered, this, [this]() { openSettings(); });

    file->addSeparator();
    auto *quitAct = file->addAction(tr("&Quit"));
    tagRexAction(quitAct, "client.quit", "App");
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
    tagRexAction(copyAct, "pane.copy", "Pane");
    copyAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(copyAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->terminal()->copyClipboard();
    });

    auto *pasteAct = edit->addAction(tr("&Paste"));
    tagRexAction(pasteAct, "pane.paste", "Pane");
    pasteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(pasteAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->terminal()->pasteClipboard();
    });

    edit->addSeparator();
    auto *findAct = edit->addAction(tr("&Find…"));
    tagRexAction(findAct, "client.find.open", "Find");
    findAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(findAct, &QAction::triggered, this, [this]() {
        // Built into QTermWidget -- no overlay to implement.
        if (auto *t = currentTerminal())
            t->terminal()->toggleShowSearchBar();
    });

    edit->addSeparator();
    auto *shotRegionAct = edit->addAction(tr("Screenshot &Region…"));
    tagRexAction(shotRegionAct, "client.screenshot.region", "Pane");
    shotRegionAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(shotRegionAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->startRegionScreenshot();
    });

    auto *shotTermAct = edit->addAction(tr("Screens&hot Terminal"));
    tagRexAction(shotTermAct, "client.screenshot.pane", "Pane");
    shotTermAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(shotTermAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            t->takeScreenshot();
    });

    auto *view = menuBar()->addMenu(tr("&View"));

    // The workspace list is rebuilt whenever it changes (and as the menu
    // opens); these actions are the window's, so they and their shortcuts
    // survive that.
    m_workspaceMenu = menuBar()->addMenu(tr("&Workspace"));
    connect(m_workspaceMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildWorkspaceMenu);
    m_newWorkspaceAct = new QAction(appIcon("plus"), tr("&New Workspace…"), this);
    connect(m_newWorkspaceAct, &QAction::triggered, this, [this]() { newWorkspace(true); });
    m_openWorkspaceMenu = new QMenu(tr("&Open Workspace"), this);
    m_openWorkspaceMenu->setIcon(appIcon("folder-open"));
    m_startWindowMenu = new QMenu(tr("&Start Window"), this);
    m_saveWorkspaceAct = new QAction(appIcon("save"),
                                     tr("&Save Workspace As…"), this);
    connect(m_saveWorkspaceAct, &QAction::triggered, this, &MainWindow::saveWorkspaceAs);
    m_editWorkspaceFileAct = new QAction(appIcon("file-pen"),
                                         tr("Edit Workspace &File"), this);
    connect(m_editWorkspaceFileAct, &QAction::triggered, this, &MainWindow::editWorkspaceFile);
    m_editWorkspaceAct = new QAction(appIcon("pencil"),
                                     tr("&Edit Workspace…"), this);
    connect(m_editWorkspaceAct, &QAction::triggered, this,
            [this]() { editWorkspace(m_currentWorkspace); });
    m_closeWorkspaceAct = new QAction(appIcon("x"), tr("&Close Workspace"), this);
    connect(m_closeWorkspaceAct, &QAction::triggered, this,
            [this]() { closeWorkspace(m_currentWorkspace); });
    // Ctrl+Shift+PgUp/PgDn: no terminal program has a use for them.
    m_nextWorkspaceAct = new QAction(tr("Ne&xt Workspace"), this);
    tagRexAction(m_nextWorkspaceAct, "client.workspace.next", "Workspace");
    m_nextWorkspaceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+PgDown")));
    connect(m_nextWorkspaceAct, &QAction::triggered, this, [this]() {
        switchWorkspace((m_currentWorkspace + 1) % m_workspaces.size());
    });
    m_prevWorkspaceAct = new QAction(tr("Pre&vious Workspace"), this);
    tagRexAction(m_prevWorkspaceAct, "client.workspace.previous", "Workspace");
    m_prevWorkspaceAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+PgUp")));
    connect(m_prevWorkspaceAct, &QAction::triggered, this, [this]() {
        const int n = m_workspaces.size();
        switchWorkspace((m_currentWorkspace - 1 + n) % n);
    });
    // Shortcuts only work for actions that are in a widget.
    addAction(m_nextWorkspaceAct);
    addAction(m_prevWorkspaceAct);
    rebuildWorkspaceMenu();
    // Filled each time it opens, since tmux sessions come and go outside.
    m_tmuxMenu = menuBar()->addMenu(tr("t&mux"));
    connect(m_tmuxMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTmuxMenu);
    // (Not left empty until then: macOS keeps an empty menu off the menu bar.)
    m_tmuxMenu->addAction(tr("No sessions running"))->setEnabled(false);
    // Saved servers of every kind, and Rex sessions' own actions; filled each
    // time it opens, as the tmux menu is.
    m_serversMenu = menuBar()->addMenu(tr("Ser&vers"));
    connect(m_serversMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildServersMenu);
    connect(&m_servers, &ServerStore::changed, this, &MainWindow::rebuildServersMenu);
    rebuildServersMenu();

    // Every action by name. Ctrl+Shift+A, as JetBrains' "Find Action": free
    // in terminals, as the other Ctrl+Shift letters here.
    auto *paletteAct = view->addAction(tr("Command &Palette…"));
    paletteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    tagRexAction(paletteAct, "client.palette.toggle", "App");
    connect(paletteAct, &QAction::triggered, this, &MainWindow::showCommandPalette);
    view->addSeparator();

#ifdef PUDDLEJUMPER_REX
    // The scripts file (Settings > Terminal > Rex) again, after editing it:
    // in the command palette (and bindable from init.lua), not a menu.
    auto *resendScripts = new QAction(tr("Resend Rex Scripts"), this);
    tagRexAction(resendScripts, "client.scripts.resend", "Rex");
    addAction(resendScripts);
    connect(resendScripts, &QAction::triggered, this, &MainWindow::resendRexScripts);
#endif

    auto *zoomIn = view->addAction(tr("Zoom &In"));
    tagRexAction(zoomIn, "client.font.increase", "View");
    zoomIn->setShortcut(QKeySequence(QStringLiteral("Ctrl+=")));
    connect(zoomIn, &QAction::triggered, this, [this]() { zoomBy(+1); });

    auto *zoomOut = view->addAction(tr("Zoom &Out"));
    tagRexAction(zoomOut, "client.font.decrease", "View");
    zoomOut->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
    connect(zoomOut, &QAction::triggered, this, [this]() { zoomBy(-1); });

    auto *zoomReset = view->addAction(tr("&Reset Zoom"));
    tagRexAction(zoomReset, "client.font.reset", "View");
    zoomReset->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(zoomReset, &QAction::triggered, this, [this]() {
        m_settings.setFontSize(13.0);
    });

    view->addSeparator();
    auto *splitRightAct = view->addAction(tr("Split &Right"));
    tagRexAction(splitRightAct, "pane.split.right", "Pane");
    splitRightAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(splitRightAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            splitPane(t, Qt::Horizontal);
    });
    auto *splitDownAct = view->addAction(tr("Split &Down"));
    tagRexAction(splitDownAct, "pane.split.down", "Pane");
    splitDownAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(splitDownAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            splitPane(t, Qt::Vertical);
    });
    auto *closePaneAct = view->addAction(tr("Close Pa&ne"));
    tagRexAction(closePaneAct, "pane.close", "Pane");
    closePaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+X")));
    connect(closePaneAct, &QAction::triggered, this, [this]() {
        if (auto *t = currentTerminal())
            closePane(t);
    });
#ifdef PUDDLEJUMPER_REX
    // A Rex pane or window closed in the last minute or so comes back.
    auto *undoCloseAct = view->addAction(tr("&Undo Close"));
    tagRexAction(undoCloseAct, "session.undo_close", "Pane");
    undoCloseAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+T")));
    undoCloseAct->setStatusTip(tr("Reopen the Rex pane or window closed most recently"));
    connect(undoCloseAct, &QAction::triggered, this, [this]() {
        TerminalWidget *t = currentTerminal();
        RexSession *rex = t ? rexSessionFor(t) : nullptr;
        if (!rex) {
            statusBar()->showMessage(tr("Undo Close reopens Rex panes; this tab isn't one"), 3000);
            return;
        }
        rex->undoClose([this, guard = QPointer<MainWindow>(this)](const QString &error) {
            if (guard && !error.isEmpty())
                statusBar()->showMessage(tr("Nothing to undo: %1").arg(error), 3000);
        });
    });
    // Rex panes can float over their window, and dock back.
    auto *floatPaneAct = view->addAction(tr("&Float or Dock Pane"));
    tagRexAction(floatPaneAct, "pane.float", "Pane");
    floatPaneAct->setStatusTip(tr("Float a Rex pane over its window, or dock a floating one back"));
    connect(floatPaneAct, &QAction::triggered, this, [this]() {
        TerminalWidget *t = currentTerminal();
        RexSession *rex = t ? rexSessionFor(t) : nullptr;
        if (!rex) {
            statusBar()->showMessage(tr("Only Rex panes can float"), 3000);
            return;
        }
        if (rex->isFloating(t))
            rex->dockPane(t);
        else
            rex->floatPane(t);
    });
    auto *zoomPaneAct = view->addAction(tr("&Zoom Pane"));
    tagRexAction(zoomPaneAct, "pane.zoom", "Pane");
    zoomPaneAct->setCheckable(true);
    zoomPaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
    zoomPaneAct->setStatusTip(tr("Let a Rex pane fill its window, or put the others back"));
    connect(zoomPaneAct, &QAction::triggered, this, [this]() {
        TerminalWidget *t = currentTerminal();
        if (RexSession *rex = t ? rexSessionFor(t) : nullptr)
            rex->toggleZoom(t);
        else
            statusBar()->showMessage(tr("Only Rex panes can zoom"), 3000);
    });
    auto *swapPaneAct = view->addAction(tr("S&wap with Next Pane"));
    tagRexAction(swapPaneAct, "pane.swap_next", "Pane");
    swapPaneAct->setStatusTip(tr("Swap a Rex pane with the next one in its window"));
    connect(swapPaneAct, &QAction::triggered, this, [this]() {
        TerminalWidget *t = currentTerminal();
        if (RexSession *rex = t ? rexSessionFor(t) : nullptr)
            rex->swapWithNext(t);
        else
            statusBar()->showMessage(tr("Only Rex panes can swap"), 3000);
    });
    // The pane actions stay visible and enabled: hiding or disabling one
    // would kill its shortcut until the menu next opened over a Rex pane.
    // Into another of the session's windows: filled as the menu opens.
    auto *movePaneMenu = view->addMenu(tr("&Move Pane to Window"));
    connect(view, &QMenu::aboutToShow, movePaneMenu, [this, movePaneMenu]() {
        movePaneMenu->clear();
        TerminalWidget *t = currentTerminal();
        RexSession *rex = t ? rexSessionFor(t) : nullptr;
        movePaneMenu->menuAction()->setVisible(rex != nullptr);
        if (!rex)
            return;
        QPointer<RexSession> session(rex);
        QPointer<TerminalWidget> pane(t);
        for (TerminalTab *tab : rex->tabs()) {
            if (tab->contains(t))
                continue;
            QPointer<TerminalTab> to(tab);
            movePaneMenu->addAction(rex->windowName(tab), this, [session, pane, to]() {
                if (session && pane && to)
                    session->movePane(pane, to);
            });
        }
        movePaneMenu->setEnabled(!movePaneMenu->isEmpty());
    });
    connect(view, &QMenu::aboutToShow, zoomPaneAct, [this, zoomPaneAct]() {
        TerminalWidget *t = currentTerminal();
        RexSession *rex = t ? rexSessionFor(t) : nullptr;
        zoomPaneAct->setChecked(rex && rex->isZoomed(t));
    });
    connect(view, &QMenu::aboutToShow, floatPaneAct, [this, floatPaneAct]() {
        TerminalWidget *t = currentTerminal();
        RexSession *rex = t ? rexSessionFor(t) : nullptr;
        floatPaneAct->setText(rex && rex->isFloating(t) ? tr("&Dock Pane") : tr("&Float Pane"));
    });
#endif
    auto *nextPaneAct = view->addAction(tr("Ne&xt Pane"));
    tagRexAction(nextPaneAct, "pane.focus_next", "Pane");
    nextPaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connect(nextPaneAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->cyclePane(+1);
    });
    auto *prevPaneAct = view->addAction(tr("Pre&vious Pane"));
    tagRexAction(prevPaneAct, "pane.focus_previous", "Pane");
    prevPaneAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")));
    connect(prevPaneAct, &QAction::triggered, this, [this]() {
        if (auto *tab = currentTab())
            tab->cyclePane(-1);
    });

    view->addSeparator();
    auto *toggleLayout = view->addAction(tr("&Thumbnail Sidebar"));
    tagRexAction(toggleLayout, "client.sidebar.toggle", "View");
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
    newAct->setIcon(appIcon("plus"));
    closeAct->setIcon(appIcon("x"));
    splitRightAct->setIcon(appIcon("columns-2"));
    splitDownAct->setIcon(appIcon("rows-2"));
    closePaneAct->setIcon(appIcon("square-x"));
    copyAct->setIcon(appIcon("copy"));
    pasteAct->setIcon(appIcon("clipboard-paste"));
    findAct->setIcon(appIcon("search"));
    shotRegionAct->setIcon(appIcon("camera"));
    shotTermAct->setIcon(appIcon("fullscreen"));
    zoomOut->setIcon(appIcon("zoom-out"));
    zoomIn->setIcon(appIcon("zoom-in"));
    toggleLayout->setIcon(appIcon("panel-left"));
    settingsAct->setIcon(appIcon("settings"));
    for (QAction *act : {newAct, closeAct, splitRightAct, splitDownAct, closePaneAct,
                         nextPaneAct, prevPaneAct, copyAct, pasteAct, findAct,
                         shotRegionAct, shotTermAct, zoomOut, zoomIn, zoomReset,
                         toggleLayout, settingsAct})
        setToolTipWithShortcut(act);

    auto *bar = addToolBar(tr("Main Toolbar"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setFloatable(false);
    // The same size everywhere: styles disagree (macOS asks for 32px, Breeze
    // 22px), and the Lucide set is drawn for small sizes.
    bar->setIconSize(QSize(22, 22));

    // The workspace first: everything else acts within it.
    m_workspaceButton = new QToolButton(bar);
    m_workspaceButton->setMenu(m_workspaceMenu);
    m_workspaceButton->setPopupMode(QToolButton::InstantPopup);
    m_workspaceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_workspaceButton->setToolTip(tr("Workspace: switch, add or edit"));
    bar->addWidget(m_workspaceButton);
    bar->addSeparator();

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

    // What needs you, in every tab and workspace: a count, and a menu of them.
    m_inboxMenu = new QMenu(this);
    connect(m_inboxMenu, &QMenu::aboutToShow, this, &MainWindow::fillInboxMenu);
    m_inboxButton = new QToolButton(bar);
    m_inboxButton->setIcon(appIcon("bell"));
    m_inboxButton->setMenu(m_inboxMenu);
    m_inboxButton->setPopupMode(QToolButton::InstantPopup);
    m_inboxButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->addWidget(m_inboxButton);
    updateInbox();

    bar->addAction(settingsAct);

    unifyTitleBar(this, bar);

    view->addSeparator();
    QAction *showBar = bar->toggleViewAction();
    showBar->setText(tr("Show &Toolbar"));
    view->addAction(showBar);
}

void MainWindow::buildStatusBar()
{
    m_statusDir = new QLabel(this);
    m_statusDir->setTextFormat(Qt::PlainText);   // titles and programs' messages are never markup
    m_statusInfo = new QLabel(this);
    statusBar()->addWidget(m_statusDir, 1);

    m_screenshotNotice = new QLabel(this);
    m_screenshotNotice->setTextFormat(Qt::RichText);
    m_screenshotNotice->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_screenshotNotice->setFocusPolicy(Qt::NoFocus);
    m_screenshotNotice->hide();
    connect(m_screenshotNotice, &QLabel::linkActivated, this, &MainWindow::saveScreenshot);
    statusBar()->addPermanentWidget(m_screenshotNotice);

    m_projectNotice = new QLabel(this);
    m_projectNotice->setTextFormat(Qt::RichText);
    m_projectNotice->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_projectNotice->setFocusPolicy(Qt::NoFocus);
    m_projectNotice->hide();
    connect(m_projectNotice, &QLabel::linkActivated, this, [this]() {
        if (!m_projectNoticeFile.isEmpty())
            openWorkspaceFile(m_projectNoticeFile);
    });
    statusBar()->addPermanentWidget(m_projectNotice);

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
    const QString suggested = dir + QStringLiteral("/puddlejumper-")
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

Tab *MainWindow::currentPage() const
{
    return qobject_cast<Tab *>(m_tabs->currentWidget());
}

Tab *MainWindow::pageAt(int index) const
{
    if (index < 0 || index >= m_tabs->count())
        return nullptr;
    return qobject_cast<Tab *>(m_tabs->widget(index));
}

TerminalTab *MainWindow::currentTab() const
{
    return qobject_cast<TerminalTab *>(currentPage());
}

TerminalTab *MainWindow::tabAt(int index) const
{
    return qobject_cast<TerminalTab *>(pageAt(index));
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
    for (TerminalTab *tab : allTabs())
        out += tab->panes();
    return out;
}

QList<Tab *> MainWindow::allPages() const
{
    QList<Tab *> out;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *tab = pageAt(i))
            out << tab;
    }
    for (const Workspace &w : m_workspaces) {
        for (const QPointer<Tab> &tab : w.parked) {
            if (tab)
                out << tab;
        }
    }
    return out;
}

QList<TerminalTab *> MainWindow::allTabs() const
{
    QList<TerminalTab *> out;
    for (Tab *page : allPages()) {
        if (auto *tab = qobject_cast<TerminalTab *>(page))
            out << tab;
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

int MainWindow::addTab(Tab *tab, const QString &text, const QString &toolTip,
                       int workspace)
{
    if (auto *terminal = qobject_cast<TerminalTab *>(tab)) {
        wireTab(terminal);
        for (TerminalWidget *pane : terminal->panes())
            wirePane(pane);
    } else if (auto *vnc = qobject_cast<VncTab *>(tab)) {
        wireVncTab(vnc);
    }
#ifdef PUDDLEJUMPER_WEB
    else if (auto *web = qobject_cast<WebTab *>(tab)) {
        wireWebTab(web);
    }
#endif
    tab->setProperty(kQuietUntilProperty, QDateTime::currentMSecsSinceEpoch() + 3000);
    if (workspace >= 0 && workspace < m_workspaces.size() && workspace != m_currentWorkspace) {
        // For a workspace not on screen: parked until it is. Its text and
        // tooltip are set from its pane when it is shown.
        tab->setParent(m_parking);
        m_workspaces[workspace].parked << tab;
        return -1;
    }
    const int index = m_tabs->addTab(tab, text);
    m_tabs->setTabToolTip(index, toolTip);
    syncTabTitles();
    return index;
}

void MainWindow::wireTab(TerminalTab *tab)
{
    connect(tab, &TerminalTab::paneAdded, this, &MainWindow::wirePane);
    connect(tab, &TerminalTab::activePaneChanged, this, [this, tab]() {
        syncTabText(tab);
        if (tab == currentTab())
            updateStatus();
    });
    // The last pane closed (its shell exited, or Close Pane): the tab goes too.
    connect(tab, &TerminalTab::empty, this, [this, tab]() { removeTab(tab); });
}

void MainWindow::openVnc(const QString &address, const QString &user, bool reducedColour, bool adaptive)
{
    QString host;
    quint16 port;
    if (!VncView::parseAddress(address, &host, &port)) {
        QMessageBox::warning(this, tr("New VNC Tab"), tr("“%1” isn't an address.").arg(address));
        return;
    }
    auto *tab = new VncTab(host, port, reducedColour);
    tab->setAdaptiveQuality(adaptive);
    if (!user.isEmpty())
        tab->setUsername(user);
    const int index = addTab(tab, tab->title(), QString());
    m_tabs->setCurrentIndex(index);
    syncTabText(tab);
    tab->focusContent();
}

void MainWindow::openRdp(const QString &address, const QString &user)
{
#ifdef PUDDLEJUMPER_RDP
    QString host;
    quint16 port;
    if (!RdpClient::parseAddress(address, &host, &port)) {
        QMessageBox::warning(this, tr("New RDP Tab"), tr("“%1” isn't an address.").arg(address));
        return;
    }
    auto *tab = new VncTab(host, port, false, nullptr, new RdpClient);
    if (!user.isEmpty())
        tab->setUsername(user);
    const int index = addTab(tab, tab->title(), QString());
    m_tabs->setCurrentIndex(index);
    syncTabText(tab);
    tab->focusContent();
#else
    QMessageBox::warning(this, tr("New RDP Tab"),
                         tr("This build has no RDP support (%1).").arg(address));
#endif
}

void MainWindow::openWeb(const QUrl &url)
{
#ifdef PUDDLEJUMPER_WEB
    auto *tab = new WebTab(url);
    const int index = addTab(tab, tab->title(), QString());
    m_tabs->setCurrentIndex(index);
    syncTabText(tab);
    tab->focusContent();
#else
    QMessageBox::warning(this, tr("New Web Tab"),
                         tr("This build has no web tabs (%1).").arg(url.toDisplayString()));
#endif
}

void MainWindow::wireWebTab(WebTab *tab)
{
#ifdef PUDDLEJUMPER_WEB
    connect(tab, &WebTab::changed, this, [this, tab]() {
        syncTabText(tab);
        if (const int i = m_tabs->indexOf(tab); i >= 0)
            updateCardDetails(i);
        if (tab == currentPage())
            updateStatus();
    });
    // What a page opens in a new window: a tab beside it, in its workspace.
    tab->setNewTabHandler([this, tab]() {
        auto *made = new WebTab;
        const int index = addTab(made, made->title(), QString(), workspaceOf(tab));
        if (index >= 0) {
            m_tabs->setCurrentIndex(index);
            syncTabText(made);
        }
        return made;
    });
#else
    Q_UNUSED(tab);
#endif
}

void MainWindow::openLink(const QUrl &url)
{
#ifdef PUDDLEJUMPER_WEB
    const QString scheme = url.scheme();
    if ((scheme == QLatin1String("http") || scheme == QLatin1String("https")) && WebTab::isLocal(url)) {
        openWeb(url);
        return;
    }
#endif
    QDesktopServices::openUrl(url);
}

void MainWindow::reloadAfterCommands()
{
#ifdef PUDDLEJUMPER_WEB
    QList<WebTab *> watching;
    for (Tab *page : allPages()) {
        if (auto *web = qobject_cast<WebTab *>(page); web && web->reloadAfterCommands())
            watching << web;
    }
    if (watching.isEmpty())
        return;
    // A command is seen running at one look and gone at the next (one
    // shorter than the looks apart may be missed, and doesn't matter).
    for (TerminalWidget *pane : allPanes()) {
        const bool running = pane->hasRunningProcess();
        const bool was = pane->property(kRunningProperty).toBool();
        pane->setProperty(kRunningProperty, running);
        if (!was || running)
            continue;
        const int workspace = workspaceOf(tabOf(pane));
        for (WebTab *web : std::as_const(watching)) {
            if (workspaceOf(web) == workspace)
                web->reload();
        }
    }
#endif
}

void MainWindow::wireVncTab(VncTab *tab)
{
    tab->view()->setShortcutFilter([this](const QKeyEvent *event) { return isAppShortcut(event); });
    connect(tab, &VncTab::changed, this, [this, tab]() {
        syncTabText(tab);
        if (const int i = m_tabs->indexOf(tab); i >= 0)
            updateCardDetails(i);
        if (tab == currentPage())
            updateStatus();
    });
}

bool MainWindow::isAppShortcut(const QKeyEvent *event) const
{
    const QKeySequence key(event->keyCombination());
    // The window's own actions, and the menus' (submenus' included).
    QList<QAction *> actions = this->actions() + menuBar()->actions();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        if (QMenu *menu = actions.at(i)->menu())
            actions += menu->actions();
    }
    for (const QAction *action : std::as_const(actions)) {
        if (!action->isEnabled())
            continue;
        for (const QKeySequence &shortcut : action->shortcuts()) {
            if (shortcut.matches(key) == QKeySequence::ExactMatch)
                return true;
        }
    }
    return false;
}

void MainWindow::syncTabText(Tab *page)
{
    const int i = m_tabs->indexOf(page);
    if (i < 0)
        return;

#ifdef PUDDLEJUMPER_WEB
    if (auto *web = qobject_cast<WebTab *>(page)) {
        const QString title = web->title();
        const QString name = !web->customTitle().isEmpty() ? web->customTitle() : title;
        m_tabs->setTabText(i, name);
        m_tabs->setTabToolTip(i, tr("Web — %1\n%2").arg(title, web->details()));
        m_sidebar->setTitle(i, name);
        m_tabStrip->setTitle(i, name);
        m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
        if (web == currentPage())
            setWindowTitle(tr("%1 — PuddleJumper").arg(name));
        return;
    }
#endif
    if (auto *vnc = qobject_cast<VncTab *>(page)) {
        const QString title = vnc->title();
        const QString name = !vnc->customTitle().isEmpty() ? vnc->customTitle() : title;
        m_tabs->setTabText(i, name);
        m_tabs->setTabToolTip(i, tr("VNC — %1\n%2").arg(title, vnc->details()));
        m_sidebar->setTitle(i, name);
        m_tabStrip->setTitle(i, name);
        m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
        if (vnc == currentPage())
            setWindowTitle(tr("%1 — PuddleJumper").arg(name));
        return;
    }

    auto *tab = qobject_cast<TerminalTab *>(page);
    TerminalWidget *pane = tab ? tab->activePane() : nullptr;
    if (!pane)
        return;
    const QString title = pane->sessionTitle();

#ifdef PUDDLEJUMPER_REX
    // A Rex tab likewise, after its window.
    if (auto *rex = rexSessionFor(tab)) {
        const QString name = rex->windowName(tab);
        m_tabs->setTabText(i, name);
        m_sidebar->setTitle(i, name);
        m_tabStrip->setTitle(i, name);
        m_tabs->setTabToolTip(i, title.isEmpty()
                                     ? tr("Rex %1 — %2").arg(rex->label(), name)
                                     : tr("Rex %1 — %2\n%3").arg(rex->label(), name, title));
        m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
        return;
    }
#endif
    // A tmux tab is named after its window, as in tmux itself; the active
    // pane's title goes in the tooltip and window title.
    if (auto *session = tmuxSessionFor(tab)) {
        const QString name = session->windowName(tab);
        m_tabs->setTabText(i, name);
        m_sidebar->setTitle(i, name);
        m_tabStrip->setTitle(i, name);
        m_tabs->setTabToolTip(i, title.isEmpty()
                                     ? tr("tmux %1 — %2").arg(session->label(), name)
                                     : tr("tmux %1 — %2\n%3").arg(session->label(), name, title));
        m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
        if (tab == currentTab())
            setWindowTitle(tr("%1 — tmux %2 — PuddleJumper")
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
    m_tabStrip->setTitle(i, name);
    m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
    if (tab == currentTab())
        setWindowTitle(title.isEmpty() ? QStringLiteral("PuddleJumper")
                                       : tr("%1 — PuddleJumper").arg(title));
}

void MainWindow::wirePane(TerminalWidget *term)
{
    connect(term, &TerminalWidget::linkOpened, this, &MainWindow::openLink);

    // A pane's title names its tab while it is the tab's active pane.
    connect(term, &TerminalWidget::titleChanged, this, [this, term]() {
        auto *tab = tabOf(term);
        if (tab && tab->activePane() == term)
            syncTabText(tab);
    });

    connect(term, &TerminalWidget::sessionFinished, this, [this, term]() {
        closePane(term);
    });

    // What a program here says it's doing; gone with the pane.
    connect(term->programStatus(), &ProgramStatus::changed, this, [this, term]() {
        updateProgramStatus(tabOf(term));
    });
    connect(term->programStatus(), &QObject::destroyed, this,
            [this, tab = QPointer<TerminalTab>(tabOf(term))]() {
        QTimer::singleShot(0, this, [this, tab]() {
            if (tab)
                updateProgramStatus(tab);
        });
    });

    // The status bar's columns and lines. (Queued: the terminal inside takes
    // its new size after the pane does.)
    connect(term, &TerminalWidget::resized, this, [this, term]() {
        if (term == currentTerminal())
            updateStatus();
    }, Qt::QueuedConnection);

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
        markTabBusy(tabOf(term));
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
    auto markIfBackground = [this, term]() { markTabBusy(tabOf(term)); };
    connect(term, &TerminalWidget::activityDetected, this, markIfBackground);
    connect(term, &TerminalWidget::silenceDetected,  this, markIfBackground);
}

void MainWindow::splitPane(TerminalWidget *pane, Qt::Orientation orientation)
{
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(pane)) {
        rex->splitPane(pane, orientation);
        return;
    }
#endif
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
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(pane)) {
        if (confirmRexClose(rex, tabOf(pane), true))
            rex->closePane(pane);   // the view change removes it
        return;
    }
#endif
    if (auto *session = tmuxSessionFor(pane)) {
        if (!confirmTmuxClose(session, tabOf(pane)))
            return;
        session->killPane(pane);   // the layout change removes it
        return;
    }
    if (auto *tab = tabOf(pane))
        tab->removePane(pane);   // the last pane takes the tab with it (empty())
}

void MainWindow::removeTab(Tab *tab)
{
    scheduleUpdateInbox();
    const int index = m_tabs->indexOf(tab);
    if (index < 0) {
        // Parked in another workspace (its shell exited, or its tmux window
        // closed), or in one that has been closed.
        if (!tab)
            return;
        for (Workspace &w : m_workspaces)
            w.parked.removeAll(tab);
        tab->deleteLater();
        return;
    }
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
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(currentTab())) {
        rex->newWindow();
        return;
    }
#endif
    newTabWithProfile(QString());
}

TmuxSession *MainWindow::tmuxSessionFor(const Tab *page) const
{
    const auto *tab = qobject_cast<const TerminalTab *>(page);
    if (!tab)
        return nullptr;
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
    m_tmuxMenu->addAction(appIcon("plus"), tr("&New Session…"),
                          this, &MainWindow::newTmuxSession);
    m_tmuxMenu->addAction(appIcon("server"),
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
    if (h.isEmpty())
        return;
    attachTmuxHost(h, session->text().trimmed());
}

void MainWindow::rebuildServersMenu()
{
    m_serversMenu->clear();
    const QList<SavedServer> servers = m_servers.servers();
    if (servers.isEmpty())
        m_serversMenu->addAction(tr("No saved servers"))->setEnabled(false);
    for (const SavedServer &s : servers) {
        const char *icon = s.kind == SavedServer::Rex    ? "server"
                           : s.kind == SavedServer::Tmux ? "square-terminal"
                           : s.kind == SavedServer::Web  ? "code-xml"
                                                         : "app-window";
        QAction *act = m_serversMenu->addAction(appIcon(icon), s.title());
        act->setToolTip(SavedServer::kindName(s.kind) + QStringLiteral(" · ") + s.address);
        act->setStatusTip(act->toolTip());
        bool available = true;
#ifndef PUDDLEJUMPER_REX
        available = available && s.kind != SavedServer::Rex;
#endif
#ifndef PUDDLEJUMPER_RDP
        available = available && s.kind != SavedServer::Rdp;
#endif
#ifndef PUDDLEJUMPER_WEB
        available = available && s.kind != SavedServer::Web;
#endif
        act->setEnabled(available);
        const QString id = s.id;
        connect(act, &QAction::triggered, this, [this, id]() { openSavedServer(m_servers.server(id)); });
    }
    m_serversMenu->addSeparator();
#ifdef PUDDLEJUMPER_REX
    m_serversMenu->addAction(appIcon("plus"), tr("Connect to &Rex Server…"), this, &MainWindow::newRexTab);
    if (RexSession *rex = rexSessionFor(currentTab())) {
        QPointer<RexSession> session(rex);
        // The server's own actions (from its init.lua, and the terminal's).
        if (!rex->keymap().actions().isEmpty()) {
            auto *actionsMenu = m_serversMenu->addMenu(tr("Rex &Actions"));
            QHash<QString, QMenu *> byCategory;
            for (const RexKeymap::Action &a : rex->keymap().actions()) {
                QMenu *into = actionsMenu;
                if (!a.category.isEmpty()) {
                    if (!byCategory.contains(a.category))
                        byCategory.insert(a.category, actionsMenu->addMenu(a.category));
                    into = byCategory.value(a.category);
                }
                QAction *item = into->addAction(a.title);
                item->setToolTip(a.description.isEmpty() ? a.name : a.description);
                const QStringList keys = rex->keymap().keysFor(a.name);
                if (!keys.isEmpty())
                    item->setText(a.title + QLatin1Char('\t') + RexKeymap::display(keys));
                const QString name = a.name;
                connect(item, &QAction::triggered, this, [this, session, name]() {
                    if (session)
                        session->runAction(name, {}, currentTerminal(), QStringLiteral("menu"));
                });
            }
        }
        m_serversMenu->addAction(tr("&Detach Rex Session “%1”").arg(rex->label()), this, [session]() {
            if (session)
                session->detach();
        });
        m_serversMenu->addAction(tr("Re&name Rex Session “%1”…").arg(rex->label()), this, [this, session]() {
            if (!session)
                return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Rex Session"), tr("Session name:"),
                                                       QLineEdit::Normal, session->label(), &ok).trimmed();
            if (ok && !name.isEmpty() && session)
                session->renameSession(name);
        });
        m_serversMenu->addAction(tr("&End Rex Session “%1”…").arg(rex->label()), this, [this, session]() {
            if (!session)
                return;
            if (QMessageBox::question(this, tr("End Rex Session?"),
                                      tr("End Rex session “%1”? Its programs are stopped on %2.")
                                          .arg(session->label(), session->server().authority()))
                    == QMessageBox::Yes
                && session)
                session->destroySession();
        });
    }
    m_serversMenu->addSeparator();
#endif
    m_serversMenu->addAction(appIcon("settings"), tr("&Edit Servers…"), this,
                             [this]() { openSettings(SettingsDialog::Servers); });
}

void MainWindow::openSavedServer(const SavedServer &s)
{
    if (s.id.isEmpty() || s.address.isEmpty())
        return;
    QString text = s.address;
    if ((s.kind == SavedServer::Rex || s.kind == SavedServer::Web) && !text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("http://"));
    switch (s.kind) {
    case SavedServer::Rex:
#ifdef PUDDLEJUMPER_REX
        chooseRexSession(QUrl(text));
#endif
        break;
    case SavedServer::Tmux:
        attachTmuxHost(s.address, s.session);
        break;
    case SavedServer::Vnc:
        openVnc(s.address, s.user, s.reducedColour, s.adaptive);
        break;
    case SavedServer::Rdp:
        openRdp(s.address, s.user);
        break;
    case SavedServer::Web:
#ifdef PUDDLEJUMPER_WEB
        openWeb(QUrl(text));
#endif
        break;
    }
}

void MainWindow::attachTmuxHost(const QString &host, const QString &session)
{
    const QString name = session.isEmpty() ? QStringLiteral("main") : session;
    // -T: no remote terminal; control mode speaks over plain pipes. BatchMode
    // makes ssh fail rather than wait for a password nobody can type.
    attachTmux(QStringLiteral("%1: %2").arg(host, name), {
        QStringLiteral("ssh"), QStringLiteral("-T"), QStringLiteral("-o"),
        QStringLiteral("BatchMode=yes"), host,
        QStringLiteral("tmux -u -C new-session -A -s ") + shellQuote(name)});
}

void MainWindow::attachTmux(const QString &label, const QStringList &command,
                            const QString &workspaceId, bool show)
{
    for (TmuxSession *s : std::as_const(m_tmuxSessions)) {
        if (s->label() != label)
            continue;
        qCInfo(lcTmux) << "attach" << label << "already open with" << s->tabs().size() << "tabs";
        if (!s->tabs().isEmpty()) {
            showTab(s->tabs().first());
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
    m_sessionWorkspace.insert(session, workspaceId.isEmpty()
                                           ? m_workspaces.value(m_currentWorkspace).id
                                           : workspaceId);
    auto shownAny = std::make_shared<bool>(false);

    connect(session, &TmuxSession::tabAdded, this,
            [this, session, shownAny, show](TerminalTab *tab, const QString &name) {
        // Into the session's workspace, which may not be the one on screen.
        const int index = addTab(tab, name, tr("tmux %1 — %2").arg(session->label(), name),
                                 workspaceIndex(m_sessionWorkspace.value(session)));
        if (!*shownAny) {
            *shownAny = true;
            if (index >= 0)
                m_tabs->setCurrentIndex(index);
            else if (show)
                showTab(tab);
            rebuildTmuxMenu();   // for a global menu, which may not ask again
        }
    });
    connect(session, &TmuxSession::tabRenamed, this, [this](TerminalTab *tab, const QString &) {
        syncTabText(tab);
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
        m_sessionWorkspace.remove(session);
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
    // than redrawing once its first tab is shown. Not before the window is
    // (restoring a session): its size then is a guess, and a resize to the
    // guess and another to the real size each make the shells redraw.
    if (QWidget *page = m_tabs->currentWidget(); page && page->isVisible()) {
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

    // In the workspace's folder, when it has one (and it still exists).
    const QString root = m_workspaces.value(m_currentWorkspace).root;
    auto *term = makePane(profile, !root.isEmpty() && QDir(root).exists() ? root : QString());
    auto *tab = new TerminalTab(term);

    // The tab tooltip carries the profile, which is the only cue in the
    // standard layout that a tab is not just "another terminal".
    const int index = addTab(tab, tr("Terminal"),
                             tr("%1 — %2").arg(profile.name, tr("Terminal")));
    m_tabs->setCurrentIndex(index);
    term->focusTerminal();
}

void MainWindow::newWslTab(const QString &distribution)
{
    // The default profile's looks, the distribution's shell.
    TerminalProfile profile = m_profiles.defaultProfile();
    const QStringList command = wsl::shellCommand(distribution);
    profile.shellProgram = command.first();
    profile.shellArgs = command.mid(1);
    profile.workingDirectory.reset();

    auto *term = makePane(profile, QString());
    auto *tab = new TerminalTab(term);
    const int index = addTab(tab, tr("Terminal"),
                             tr("%1 — %2").arg(tr("%1 (WSL)").arg(distribution), tr("Terminal")));
    m_tabs->setCurrentIndex(index);
    term->focusTerminal();
}

void MainWindow::closeTab(int index)
{
    if (auto *page = pageAt(index); page && !tabAt(index)) {
        removeTab(page);   // not terminals: nothing to confirm
        return;
    }
    auto *tab = tabAt(index);
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(tab)) {
        if (confirmRexClose(rex, tab, false))
            rex->closeWindow(tab);   // the tab goes when the window has
        return;
    }
#endif
    if (auto *session = tmuxSessionFor(tab)) {
        if (!confirmTmuxClose(session, tab))
            return;
        session->killWindow(tab);  // the tab goes when tmux says the window has
        return;
    }
    if (tab)
        removeTab(tab);
}

#ifdef PUDDLEJUMPER_REX
RexSession *MainWindow::rexSessionFor(const Tab *tab) const
{
    for (RexSession *s : m_rexSessions)
        if (s->ownsTab(tab))
            return s;
    return nullptr;
}

RexSession *MainWindow::rexSessionFor(const TerminalWidget *pane) const
{
    for (RexSession *s : m_rexSessions)
        if (s->ownsPane(pane))
            return s;
    return nullptr;
}

void MainWindow::newRexTab()
{
    QSettings settings;
    const QString last = settings.value(QStringLiteral("rex/lastServer"),
                                        QStringLiteral("http://127.0.0.1:51433")).toString();
    bool ok = false;
    QString text = QInputDialog::getText(this, tr("New Rex Tab"), tr("Rex server (http://host:port):"),
                                         QLineEdit::Normal, last, &ok).trimmed();
    if (!ok || text.isEmpty())
        return;
    if (!text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("http://"));
    const QUrl server(text);
    if (!server.isValid() || server.host().isEmpty()) {
        QMessageBox::warning(this, tr("Rex"), tr("“%1” is not a server address.").arg(text));
        return;
    }
    settings.setValue(QStringLiteral("rex/lastServer"), text);
    chooseRexSession(server);
}

void MainWindow::chooseRexSession(const QUrl &server)
{
    statusBar()->showMessage(tr("Connecting to %1…").arg(server.authority()));
    QPointer<MainWindow> self(this);
    RexSession::listSessions(server, this,
                             [self, server](const QList<QPair<QString, QString>> &sessions, const QString &error) {
        if (!self)
            return;
        self->statusBar()->clearMessage();
        if (!error.isEmpty()) {
            QMessageBox::warning(self, tr("Rex"),
                                 tr("Could not connect to %1.").arg(server.authority())
                                     + QStringLiteral("\n\n") + error);
            return;
        }
        QStringList items{tr("New session")};
        for (const auto &s : sessions)
            items << tr("%1 (%2)").arg(s.second, s.first.section(QLatin1Char(':'), -1).left(8));
        bool chosen = false;
        const QString named = self->m_servers.rexServer(server.toString()).name;
        const QString item = QInputDialog::getItem(self, tr("New Rex Tab"),
                                                   tr("Session on %1:").arg(named.isEmpty() ? server.authority()
                                                                                            : named),
                                                   items, sessions.isEmpty() ? 0 : 1, false, &chosen);
        if (!chosen)
            return;
        const int i = int(items.indexOf(item));
        self->openRex(server, i > 0 ? sessions.at(i - 1).first : QString());
    });
}

void MainWindow::openRex(const QUrl &server, const QString &sessionId, const QString &workspaceId,
                         bool quiet)
{
    for (RexSession *s : std::as_const(m_rexSessions)) {
        if (!sessionId.isEmpty() && s->sessionId() == sessionId && s->server() == server) {
            if (!s->tabs().isEmpty())
                showTab(s->tabs().first());
            return;   // open already (or still opening)
        }
    }
    auto *session = new RexSession(server, sessionId, m_settings, m_profiles.defaultProfile(), this);
    m_rexSessions << session;
    m_rexWorkspace.insert(session, workspaceId.isEmpty() ? m_workspaces.value(m_currentWorkspace).id
                                                         : workspaceId);
    auto shownAny = std::make_shared<bool>(false);
    connect(session, &RexSession::tabAdded, this,
            [this, session, shownAny, quiet](TerminalTab *tab, const QString &name) {
        // Into the session's workspace, which may not be the one on screen.
        const int index = addTab(tab, name, tr("Rex %1 — %2").arg(session->label(), name),
                                 workspaceIndex(m_rexWorkspace.value(session)));
        if (!std::exchange(*shownAny, true) && index >= 0 && !quiet)
            m_tabs->setCurrentIndex(index);
    });
    connect(session, &RexSession::tabRenamed, this, [this](TerminalTab *tab, const QString &) {
        syncTabText(tab);
    });
    // The session's name is in every one of its tabs' text and pill.
    connect(session, &RexSession::labelChanged, this, [this, session]() {
        for (TerminalTab *tab : session->tabs()) {
            syncTabText(tab);
            const int index = m_tabs->indexOf(tab);
            if (index >= 0)
                updateCardDetails(index);
        }
    });
    connect(session, &RexSession::tabRemoved, this, &MainWindow::removeTab);
    wireRexActions(session);
    connect(session, &RexSession::ended, this, [this, session, shownAny, quiet](const QString &reason) {
        m_rexSessions.removeOne(session);
        m_rexWorkspace.remove(session);
        session->deleteLater();
        if (!*shownAny) {
            if (quiet) {
                qCInfo(lcTmux) << "Rex session" << session->sessionId() << "not restored:" << reason;
                return;
            }
            QMessageBox::warning(this, tr("Rex"),
                                 tr("Could not open a Rex session on %1.").arg(session->server().authority())
                                     + (reason.isEmpty() ? QString() : QStringLiteral("\n\n") + reason));
            return;
        }
        statusBar()->showMessage(reason.isEmpty() ? tr("Rex session %1 closed").arg(session->label())
                                                  : tr("Rex session %1: %2").arg(session->label(), reason),
                                 6000);
    });
    session->start();
}

bool MainWindow::confirmRexClose(RexSession *session, TerminalTab *tab, bool pane)
{
    // As for tmux: closing the last of a session ends it, which is rarely
    // what closing a tab here means, so detaching is offered instead. A pane
    // whose program has exited (kept to be read) just closes.
    if (session->tabs().size() != 1 || !tab || (pane && tab->panes().size() != 1))
        return true;
    if (pane && session->hasExited(tab->activePane()))
        return true;
    QMessageBox box(QMessageBox::Question, tr("Close Rex Session?"),
                    tr("“%1” is the last window of Rex session “%2”. Closing it ends the session.")
                        .arg(session->windowName(tab), session->label()),
                    QMessageBox::NoButton, this);
    box.setInformativeText(tr("Detach to keep the session running on %1, so you can attach to it "
                              "again from here or any Rex client.")
                               .arg(session->server().authority()));
    QPushButton *detach = box.addButton(tr("&Detach"), QMessageBox::AcceptRole);
    QPushButton *end = box.addButton(tr("&End Session"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(detach);
    box.exec();
    if (box.clickedButton() == detach)
        session->detach();
    return box.clickedButton() == end;
}
#endif

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
    // Pane cards change what's behind the panes, and the sidebar's handle.
    if (m_splitter && m_splitter->count() > 1)
        m_splitter->handle(1)->update();
    if (QWidget *page = m_tabs->currentWidget()) {
        page->update();
        for (QSplitterHandle *h : page->findChildren<QSplitterHandle *>())
            h->update();
    }
}

void MainWindow::updateTabLayout()
{
    const bool thumbs =
        m_settings.appearance().tabLayout == QStringLiteral("thumbnails");

    m_sidebar->setVisible(thumbs);
    // In thumbnail mode the cards carry the titles, so the strip is noise.
    m_tabStrip->setVisible(!thumbs);
    updateTabColours();

    if (thumbs) {
        // Back from being dragged shut (or never sized): give it its width.
        if (m_splitter->sizes().value(0) < m_sidebar->minimumWidth()) {
            const int saved = m_settings.sidebarWidth();
            setSidebarWidth(saved >= m_sidebar->minimumWidth() ? saved : kDefaultSidebarWidth);
        }
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
    m_tabStrip->syncTabs(titles, m_tabs->currentIndex());
    // The cards are new: put back the marks for unseen output.
    for (int i = 0; i < m_tabs->count(); ++i) {
        m_tabStrip->setTabToolTip(i, m_tabs->tabToolTip(i));
        if (auto *tab = pageAt(i); tab && tab->property(kBusyProperty).toBool()) {
            m_sidebar->setBusy(i, true);
            m_tabStrip->setBusy(i, true);
        }
        updateProgramStatus(pageAt(i));
    }
    refreshPreviews(true);
}

void MainWindow::refreshPreviews(bool all)
{
    if (m_settings.appearance().tabLayout != QStringLiteral("thumbnails"))
        return;
    const QSize size = m_sidebar->previewSize();
    const qreal dpr = devicePixelRatioF();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *tab = pageAt(i);
        if (!tab || (!all && !tab->hasNewOutput()))
            continue;
        tab->clearNewOutput();
        m_sidebar->setPreview(i, tab->previewPixmap(size, dpr));
        updateCardDetails(i);
    }
}

void MainWindow::updateCardDetails(int index)
{
    if (auto *vnc = qobject_cast<VncTab *>(pageAt(index))) {
        m_sidebar->setBadge(index, vnc->view()->client()->scheme().toUpper());
        m_sidebar->setSubtitle(index, vnc->details());
        return;
    }
#ifdef PUDDLEJUMPER_WEB
    if (auto *web = qobject_cast<WebTab *>(pageAt(index))) {
        m_sidebar->setBadge(index, tr("Web"));
        m_sidebar->setSubtitle(index, web->details());
        return;
    }
#endif
    auto *tab = tabAt(index);
    TerminalWidget *pane = tab ? tab->activePane() : nullptr;
    if (!pane)
        return;

    // tmux cards say which session they belong to; the second line is the
    // active pane's title when that says more than the window's name.
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(tab)) {
        m_sidebar->setBadge(index, tr("rex %1").arg(rex->label()));
        const QString title = pane->sessionTitle();
        m_sidebar->setSubtitle(index, title == rex->windowName(tab) ? QString() : title);
        return;
    }
#endif
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

void MainWindow::updateTabColours()
{
    if (auto *tab = currentTab()) {
        m_sidebar->setColours(tab->backgroundColour(), tab->foregroundColour());
        m_tabStrip->setColours(tab->backgroundColour(), tab->foregroundColour());
    }
    m_splitter->handle(1)->update();
}

void MainWindow::showTabMenu(int index, const QPoint &globalPos)
{
    QMenu menu(this);
    if (index >= 0) {
        menu.addAction(tr("&Rename…"), this, [this, index]() { renameTab(index); });
        menu.addSeparator();
        menu.addAction(appIcon("x"), tr("&Close Tab"), this,
                       [this, index]() { closeTab(index); });
        QAction *others = menu.addAction(tr("Close &Other Tabs"), this, [this, index]() {
            QWidget *keep = m_tabs->widget(index);
            for (int i = m_tabs->count() - 1; i >= 0; --i) {
                if (m_tabs->widget(i) != keep)
                    closeTab(i);
            }
        });
        others->setEnabled(m_tabs->count() > 1);

        // A remote screen's colour depth and a Mac's coarse-then-sharp
        // picture, for a slow link.
        if (auto *vnc = qobject_cast<VncTab *>(pageAt(index))) {
            menu.addSeparator();
            QAction *reduced = menu.addAction(tr("Reduced Colour (16-bit)"));
            reduced->setVisible(vnc->view()->client()->scheme() == QLatin1String("vnc"));
            reduced->setCheckable(true);
            reduced->setChecked(vnc->reducedColour());
            QPointer<VncTab> tab = vnc;
            connect(reduced, &QAction::toggled, this, [tab](bool on) {
                if (tab)
                    tab->setReducedColour(on);
            });
            QAction *adaptive = menu.addAction(tr("Adaptive Quality (Mac)"));
            adaptive->setVisible(reduced->isVisible());
            adaptive->setCheckable(true);
            adaptive->setChecked(vnc->adaptiveQuality());
            connect(adaptive, &QAction::toggled, this, [tab](bool on) {
                if (tab)
                    tab->setAdaptiveQuality(on);
            });
            menu.addSeparator();
        }
#ifdef PUDDLEJUMPER_WEB
        // A page to see the result of a build: reloaded as one finishes.
        if (auto *web = qobject_cast<WebTab *>(pageAt(index))) {
            menu.addSeparator();
            QAction *reload = menu.addAction(tr("Reload After &Commands"));
            reload->setCheckable(true);
            reload->setChecked(web->reloadAfterCommands());
            QPointer<WebTab> tab = web;
            connect(reload, &QAction::toggled, this, [tab](bool on) {
                if (tab)
                    tab->setReloadAfterCommands(on);
            });
            menu.addSeparator();
        }
#endif

        // A tmux session lives in one workspace, so its tabs move together.
        bool wholeSession = tmuxSessionFor(pageAt(index)) != nullptr;
#ifdef PUDDLEJUMPER_REX
        wholeSession = wholeSession || rexSessionFor(pageAt(index));
#endif
        QMenu *move = menu.addMenu(wholeSession ? tr("&Move Session to Workspace")
                                                : tr("&Move to Workspace"));
        QPointer<Tab> tab = pageAt(index);
        for (int w = 0; w < m_workspaces.size(); ++w) {
            if (w == m_currentWorkspace)
                continue;
            const QString id = m_workspaces.at(w).id;
            move->addAction(m_workspaces.at(w).icon(palette().color(QPalette::WindowText)),
                            m_workspaces.at(w).name, this, [this, tab, id]() {
                if (tab)
                    moveTabToWorkspace(tab, workspaceIndex(id));
            });
        }
        if (!move->isEmpty())
            move->addSeparator();
        move->addAction(appIcon("plus"), tr("New Workspace…"), this, [this, tab]() {
            const int w = newWorkspace(false);
            if (tab && w >= 0)
                moveTabToWorkspace(tab, w);
        });
        menu.addSeparator();
    }
    menu.addAction(appIcon("plus"), tr("&New Tab"), this, &MainWindow::newTab);
    menu.exec(globalPos);
}

void MainWindow::renameTab(int index)
{
    auto *tab = pageAt(index);
    if (!tab)
        return;
    auto *session = tmuxSessionFor(tab);
    auto *terminal = qobject_cast<TerminalTab *>(tab);
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Rename Tab"),
        session    ? tr("tmux window name:")
#ifdef PUDDLEJUMPER_REX
        : rexSessionFor(tab) ? tr("Rex window name:")
#endif
        : terminal ? tr("Tab name (leave empty to follow the terminal's title):")
                   : tr("Tab name (leave empty to follow the remote desktop's name):"),
        QLineEdit::Normal,
        session ? session->windowName(terminal)
                : (tab->customTitle().isEmpty() ? m_tabs->tabText(index) : tab->customTitle()),
        &ok).trimmed();
    if (!ok)
        return;
    if (session) {
        // tmux renames it and says so; the tab follows from that.
        if (!name.isEmpty())
            session->renameWindow(terminal, name);
        return;
    }
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(tab)) {
        if (!name.isEmpty())
            rex->renameWindow(terminal, name);   // the view change renames the tab
        return;
    }
#endif
    tab->setCustomTitle(name);
    syncTabText(tab);
}

void MainWindow::updateStatus()
{
#ifdef PUDDLEJUMPER_WEB
    if (auto *web = qobject_cast<WebTab *>(currentPage())) {
        m_statusDir->setText(tr("Web  ·  %1").arg(web->title()));
        m_statusInfo->setText(web->reloadAfterCommands()
                                  ? tr("%1  ·  reloads after commands").arg(web->details())
                                  : web->details());
        return;
    }
#endif
    if (auto *vnc = qobject_cast<VncTab *>(currentPage())) {
        m_statusDir->setText(QStringLiteral("%1  ·  %2").arg(vnc->view()->client()->scheme().toUpper(), vnc->title()));
        m_statusInfo->setText(vnc->details());
        return;
    }
    auto *term = currentTerminal();
    if (!term) {
        m_statusDir->clear();
        m_statusInfo->clear();
        return;
    }
    QString title = term->sessionTitle();
    if (auto *session = tmuxSessionFor(term))
        title = tr("tmux %1  ·  %2").arg(session->label(),
                                         title.isEmpty() ? session->windowName(tabOf(term)) : title);
#ifdef PUDDLEJUMPER_REX
    if (auto *rex = rexSessionFor(term))
        title = tr("Rex %1  ·  %2").arg(rex->label(), title.isEmpty() ? rex->windowName(tabOf(term)) : title);
#endif
    // What the program here says it's doing, and why.
    const ProgramStatusRecord status = term->programStatus()->mostPressing();
    if (ProgramStatus::rank(status) > 0) {
        QString said = ProgramStatus::label(status);
        if (!status.msg.isEmpty())
            said += QStringLiteral(": ") + status.msg;
        title = title.isEmpty() ? said : title + QStringLiteral("  ·  ") + said;
    }
    m_statusDir->setText(title);

    auto *t = term->terminal();
    // Report the tab's *effective* scheme, which a profile may have overridden.
    m_statusInfo->setText(tr("%1 × %2   ·   scrollback %3   ·   %4")
                              .arg(t->screenColumnsCount())
                              .arg(t->screenLinesCount())
                              .arg(t->historySize())
                              .arg(term->appliedColorScheme()));
}

void MainWindow::updateProgramStatus(QWidget *page)
{
    // The most pressing record of any pane in the tab.
    auto *tab = qobject_cast<TerminalTab *>(page);
    const ProgramStatusRecord best = tab ? tabStatus(tab) : ProgramStatusRecord();
    if (tab) {
        notifyProgramStatus(tab, best);   // in any workspace
        const bool blocked = best.state == ProgramStatusRecord::Blocked;
        if (tab->property(kBlocked).toBool() != blocked) {
            tab->setProperty(kBlocked, blocked);
            scheduleFloatBlockedTabs();
        }
        // Since when it has wanted this of you: a new state, record or kind
        // starts the clock again; anything less (a new message) doesn't.
        const QString key = ProgramStatus::rank(best) >= 2
            ? QStringList{QString::number(best.state), best.id, best.kind}.join(QChar(0x1f))
            : QString();
        if (tab->property(kInboxKey).toString() != key) {
            tab->setProperty(kInboxKey, key);
            tab->setProperty(kInboxSince, QDateTime::currentMSecsSinceEpoch());
        }
        scheduleUpdateInbox();
    }

    const int index = page ? m_tabs->indexOf(page) : -1;
    if (index < 0)
        return;   // gone, or in a workspace not on screen (synced when it is)

    cardstyle::Status status;
    switch (best.state) {
    case ProgramStatusRecord::Blocked: status.kind = cardstyle::Status::Blocked; break;
    case ProgramStatusRecord::Error: status.kind = cardstyle::Status::Error; break;
    case ProgramStatusRecord::Done: status.kind = cardstyle::Status::Done; break;
    case ProgramStatusRecord::Working: status.kind = cardstyle::Status::Working; break;
    default: break;
    }
    if (status.kind != cardstyle::Status::None) {
        status.label = ProgramStatus::label(best);
        status.progress = best.progress;
        // The program and what it said; markup is never read in it.
        QStringList parts;
        for (const QString &part : {best.app, best.title, best.msg}) {
            if (!part.isEmpty())
                parts << part;
        }
        status.detail = status.label + (parts.isEmpty() ? QString() : QStringLiteral(": ") + parts.join(QStringLiteral(" · ")));
    }
    m_sidebar->setStatus(index, status);
    m_tabStrip->setStatus(index, status);
    if (page == currentPage())
        updateStatus();
}

void MainWindow::scheduleFloatBlockedTabs()
{
    if (m_floatPending)
        return;
    m_floatPending = true;
    // Later, not here: this comes while the tabs are being gone through.
    QTimer::singleShot(0, this, [this]() {
        m_floatPending = false;
        floatBlockedTabs();
    });
}

void MainWindow::moveTabTo(QWidget *page, int to)
{
    const int from = m_tabs->indexOf(page);
    if (from < 0 || from == to)
        return;
    // As a drag in neither view: both follow (see the tabBar's tabMoved).
    m_tabs->tabBar()->moveTab(from, to);
}

void MainWindow::heldByHand(QWidget *page)
{
    // Moved by hand: the user has put it where they want it.
    if (page && page->property(kFloated).toBool()) {
        page->setProperty(kFloated, false);
        page->setProperty(kFloatHome, QVariant());
        page->setProperty(kFloatHeld, true);
    }
}

void MainWindow::floatBlockedTabs()
{
    if (m_switchingWorkspace)
        return;
    const bool on = m_settings.scrolling().floatBlockedTabs;
    QList<QWidget *> pages;
    for (int i = 0; i < m_tabs->count(); ++i)
        pages << m_tabs->widget(i);
    auto floatedCount = [this]() {
        int n = 0;
        for (int i = 0; i < m_tabs->count(); ++i)
            n += m_tabs->widget(i)->property(kFloated).toBool();
        return n;
    };

    // Answered (or the option turned off): back after the tab it came after,
    // else to the front of the rest.
    for (QWidget *page : std::as_const(pages)) {
        const bool blocked = page->property(kBlocked).toBool();
        if (!blocked)
            page->setProperty(kFloatHeld, false);
        if (!page->property(kFloated).toBool() || (on && blocked))
            continue;
        const auto home = page->property(kFloatHome).value<QPointer<QWidget>>();
        page->setProperty(kFloated, false);
        page->setProperty(kFloatHome, QVariant());
        const int from = m_tabs->indexOf(page);
        // (Not after one that has since floated too: that's no place.)
        const int after = home && !home->property(kFloated).toBool() ? m_tabs->indexOf(home) : -1;
        if (after < 0)
            moveTabTo(page, floatedCount());
        else
            moveTabTo(page, after < from ? after + 1 : after);
    }
    if (!on)
        return;

    // Newly waiting on you: to the front, behind those already there; not
    // the tab being looked at, which stays put until it's answered.
    for (QWidget *page : std::as_const(pages)) {
        if (!page->property(kBlocked).toBool() || page->property(kFloated).toBool()
            || page->property(kFloatHeld).toBool())
            continue;
        if (page == currentPage()) {
            page->setProperty(kFloatHeld, true);
            continue;
        }
        QPointer<QWidget> home;
        for (int i = m_tabs->indexOf(page) - 1; i >= 0 && !home; --i) {
            if (!m_tabs->widget(i)->property(kFloated).toBool())
                home = m_tabs->widget(i);
        }
        const int to = floatedCount();
        page->setProperty(kFloated, true);
        page->setProperty(kFloatHome, QVariant::fromValue(home));
        moveTabTo(page, to);
    }
}

ProgramStatusRecord MainWindow::tabStatus(TerminalTab *tab)
{
    ProgramStatusRecord best;
    for (TerminalWidget *pane : tab->panes()) {
        const ProgramStatusRecord r = pane->programStatus()->mostPressing();
        if (ProgramStatus::rank(r) > ProgramStatus::rank(best))
            best = r;
    }
    return best;
}

QList<MainWindow::InboxItem> MainWindow::inbox() const
{
    // Needing you first, then failed, then done; within each, the longest
    // waiting first.
    QList<InboxItem> items;
    for (TerminalTab *tab : allTabs()) {
        const ProgramStatusRecord r = tabStatus(tab);
        if (ProgramStatus::rank(r) >= 2)
            items.append({tab, r, tab->property(kInboxSince).toLongLong()});
    }
    std::stable_sort(items.begin(), items.end(), [](const InboxItem &a, const InboxItem &b) {
        const int ra = ProgramStatus::rank(a.record), rb = ProgramStatus::rank(b.record);
        return ra != rb ? ra > rb : a.since < b.since;
    });
    return items;
}

void MainWindow::scheduleUpdateInbox()
{
    if (m_inboxPending)
        return;
    m_inboxPending = true;
    // Later: a closed tab is still among them until it's gone.
    QTimer::singleShot(0, this, [this]() {
        m_inboxPending = false;
        updateInbox();
    });
}

void MainWindow::updateInbox()
{
    if (!m_inboxButton)
        return;
    int waiting = 0, finished = 0;
    for (const InboxItem &item : inbox())
        ++(item.record.state == ProgramStatusRecord::Blocked ? waiting : finished);
    const int total = waiting + finished;
    m_inboxButton->setText(total ? QString::number(total) : QString());
    QStringList parts;
    if (waiting)
        parts << tr("%n waiting on you", nullptr, waiting);
    if (finished)
        parts << tr("%n finished or failed", nullptr, finished);
    m_inboxButton->setToolTip(
        (parts.isEmpty() ? tr("Nothing needs you") : parts.join(QStringLiteral(", ")))
        + QStringLiteral(" (%1)").arg(m_nextNeedingYouAct->shortcut().toString(QKeySequence::NativeText)));
}

QString MainWindow::waitedFor(qint64 since)
{
    const qint64 secs = (QDateTime::currentMSecsSinceEpoch() - since) / 1000;
    if (since <= 0 || secs < 60)
        return tr("just now");
    if (secs < 3600)
        return tr("%n min", nullptr, int(secs / 60));
    return tr("%n h", nullptr, int(secs / 3600));
}

void MainWindow::fillInboxMenu()
{
    m_inboxMenu->clear();
    const QList<InboxItem> items = inbox();
    if (items.isEmpty()) {
        m_inboxMenu->addAction(tr("Nothing needs you"))->setEnabled(false);
        return;
    }
    const QFontMetrics fm(m_inboxMenu->font());
    for (const InboxItem &item : items) {
        cardstyle::Status::Kind kind = cardstyle::Status::Done;
        if (item.record.state == ProgramStatusRecord::Blocked)
            kind = cardstyle::Status::Blocked;
        else if (item.record.state == ProgramStatusRecord::Error)
            kind = cardstyle::Status::Error;
        // What, where, what it said, and for how long. Programs' text: no
        // mnemonics from it.
        QStringList parts{ProgramStatus::label(item.record), describeTab(item.tab)};
        if (!item.record.msg.isEmpty())
            parts << fm.elidedText(item.record.msg, Qt::ElideRight, 320);
        parts << waitedFor(item.since);
        QString text = parts.join(QStringLiteral("  ·  "));
        text.replace(QLatin1Char('&'), QLatin1String("&&"));
        QAction *act = m_inboxMenu->addAction(
            cardstyle::dotIcon(devicePixelRatioF(), cardstyle::statusColour(kind)), text);
        if (item.tab == currentPage()) {   // the one on screen
            QFont bold = act->font();
            bold.setBold(true);
            act->setFont(bold);
        }
        connect(act, &QAction::triggered, this, [this, tab = QPointer<TerminalTab>(item.tab)]() {
            if (tab)
                showTab(tab);
        });
    }
    m_inboxMenu->addSeparator();
    m_inboxMenu->addAction(m_nextNeedingYouAct);
}

void MainWindow::showNextNeedingYou()
{
    const QList<InboxItem> items = inbox();
    if (items.isEmpty()) {
        statusBar()->showMessage(tr("Nothing needs you"), 3000);
        return;
    }
    // The one after the tab on screen, if it's among them; else the first.
    int next = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).tab == currentPage()) {
            next = (i + 1) % items.size();
            break;
        }
    }
    showTab(items.at(next).tab);
}

QString MainWindow::notificationKey(const QWidget *tab)
{
    return QStringLiteral("tab-%1").arg(quintptr(tab), 0, 16);
}

QString MainWindow::describeTab(TerminalTab *tab) const
{
    QString name = tab->customTitle();
    if (name.isEmpty()) {
        if (TmuxSession *session = tmuxSessionFor(tab))
            name = tr("tmux %1 — %2").arg(session->label(), session->windowName(tab));
#ifdef PUDDLEJUMPER_REX
        else if (RexSession *rex = rexSessionFor(tab))
            name = tr("Rex %1 — %2").arg(rex->label(), rex->windowName(tab));
#endif
        else if (TerminalWidget *pane = tab->activePane())
            name = pane->sessionTitle();
    }
    if (name.isEmpty())
        name = tr("Terminal");
    const int w = workspaceOf(tab);
    if (m_workspaces.size() > 1 && w >= 0)
        name = tr("%1 (%2)").arg(name, m_workspaces.at(w).name);
    return name;
}

void MainWindow::notifyProgramStatus(TerminalTab *tab, const ProgramStatusRecord &record)
{

    // Only what wants the user: needing them, done, failed. Anything less
    // (working again, or seen) takes the notification away.
    if (ProgramStatus::rank(record) < 2) {
        if (!tab->property(kNotified).toString().isEmpty()) {
            tab->setProperty(kNotified, QString());
            m_notifier->withdraw(notificationKey(tab));
        }
        return;
    }
    const QString what = QStringList{QString::number(record.state), record.id, record.kind,
                                     record.app, record.title, record.msg}.join(QChar(0x1f));
    if (what == tab->property(kNotified).toString())
        return;

    // Not for the tab being looked at.
    const bool watched = isActiveWindow() && !isMinimized() && tab == currentPage();
    if (watched || !m_settings.scrolling().notifyProgramStatus) {
        tab->setProperty(kNotified, what);
        return;
    }

    // Rate-limited: a later change within the interval is looked at again
    // when it is over.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = tab->property(kNotifiedAt).toLongLong();
    if (now - last < kNotifyInterval) {
        if (!tab->property(kPending).toBool()) {
            tab->setProperty(kPending, true);
            QTimer::singleShot(kNotifyInterval - (now - last), this, [this, tab = QPointer<TerminalTab>(tab)]() {
                if (!tab)
                    return;
                tab->setProperty(kPending, false);
                updateProgramStatus(tab);
            });
        }
        return;
    }
    tab->setProperty(kNotified, what);
    tab->setProperty(kNotifiedAt, now);

    // Which tab, always: a program can't pass for another one.
    const QString title = tr("%1 — %2").arg(ProgramStatus::label(record), describeTab(tab));
    QStringList body;
    if (!record.app.isEmpty())
        body << record.app;
    if (!record.title.isEmpty())
        body << record.title;
    if (!record.msg.isEmpty())
        body << record.msg;
    m_notifier->notify(notificationKey(tab), title, body.join(QStringLiteral(": ")));
    // Waiting on the user: the taskbar or Dock asks for attention too.
    if (record.state == ProgramStatusRecord::Blocked)
        QApplication::alert(this);
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
    box.setWindowTitle(tr("Quit PuddleJumper?"));

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
        // And the WSL distributions (Windows): a shell in each.
        if (const QStringList distributions = wsl::distributions(); !distributions.isEmpty()) {
            m_newTabWithProfileMenu->setEnabled(true);
            m_newTabWithProfileMenu->addSeparator();
            for (const QString &name : distributions) {
                QAction *act = m_newTabWithProfileMenu->addAction(tr("%1 (WSL)").arg(name));
                connect(act, &QAction::triggered, this, [this, name]() { newWslTab(name); });
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
        m_settingsDialog = new SettingsDialog(m_settings, m_profiles, this, &m_servers);
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
    saveForQuit();
    QMainWindow::closeEvent(event);
}

void MainWindow::saveForQuit()
{
    m_settings.finaliseLiveSchemes();
    m_settings.saveWindowGeometry(saveGeometry(), saveState());
    saveSession();
}

// ---- workspaces ---------------------------------------------------------------

int MainWindow::workspaceOf(const Tab *tab) const
{
    if (!tab)
        return -1;
    if (m_tabs->indexOf(const_cast<Tab *>(tab)) >= 0)
        return m_currentWorkspace;
    for (int i = 0; i < m_workspaces.size(); ++i) {
        for (const QPointer<Tab> &t : m_workspaces.at(i).parked) {
            if (t == tab)
                return i;
        }
    }
    return -1;
}

int MainWindow::workspaceIndex(const QString &id) const
{
    for (int i = 0; i < m_workspaces.size(); ++i) {
        if (m_workspaces.at(i).id == id)
            return i;
    }
    return -1;
}

void MainWindow::switchWorkspace(int index)
{
    if (index < 0 || index >= m_workspaces.size() || index == m_currentWorkspace)
        return;

    m_switchingWorkspace = true;
    Workspace &from = m_workspaces[m_currentWorkspace];
    from.parked.clear();
    from.currentTab = qMax(0, m_tabs->currentIndex());
    while (m_tabs->count() > 0) {
        Tab *tab = pageAt(0);
        m_tabs->removeTab(0);
        if (tab) {
            tab->setParent(m_parking);
            from.parked << tab;
        }
    }

    m_currentWorkspace = index;
    Workspace &to = m_workspaces[index];
    to.unseenOutput = false;
    for (const QPointer<Tab> &tab : std::as_const(to.parked)) {
        if (tab)
            m_tabs->addTab(tab, QString());
    }
    to.parked.clear();
    m_switchingWorkspace = false;

    if (m_tabs->count() == 0) {
        newTab();   // a new or emptied workspace starts with a tab
    } else {
        for (int i = 0; i < m_tabs->count(); ++i)
            syncTabText(pageAt(i));
        const int current = qBound(0, to.currentTab, m_tabs->count() - 1);
        m_tabs->setCurrentIndex(current);
        syncTabTitles();
        onCurrentTabChanged(current);
    }
    updateWorkspaceChrome();
}

void MainWindow::showTab(Tab *tab)
{
    const int w = workspaceOf(tab);
    if (w < 0)
        return;
    switchWorkspace(w);
    m_tabs->setCurrentWidget(tab);
}

int MainWindow::newWorkspace(bool show)
{
    Workspace ws;
    ws.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!WorkspaceDialog::edit(this, tr("New Workspace"), ws))
        return -1;
    m_workspaces << ws;
    const int index = m_workspaces.size() - 1;
    if (show)
        switchWorkspace(index);
    else
        updateWorkspaceChrome();
    return index;
}

void MainWindow::editWorkspace(int index)
{
    if (index < 0 || index >= m_workspaces.size())
        return;
    Workspace ws = m_workspaces.at(index);
    if (!WorkspaceDialog::edit(this, tr("Edit Workspace"), ws))
        return;
    m_workspaces[index].name = ws.name;
    m_workspaces[index].root = ws.root;
    m_workspaces[index].colour = ws.colour;
    updateWorkspaceChrome();
}

void MainWindow::closeWorkspace(int index)
{
    if (index < 0 || index >= m_workspaces.size() || m_workspaces.size() < 2)
        return;
    const QString name = m_workspaces.at(index).name;

    // Its tabs, and what closing them would stop.
    QList<Tab *> tabs;
    for (Tab *tab : allPages()) {
        if (workspaceOf(tab) == index)
            tabs << tab;
    }
    QStringList running;
    QList<TmuxSession *> sessions;
#ifdef PUDDLEJUMPER_REX
    QList<RexSession *> rexSessions;
#endif
    for (Tab *tab : std::as_const(tabs)) {
        if (TmuxSession *session = tmuxSessionFor(tab)) {
            if (!sessions.contains(session))
                sessions << session;
            continue;
        }
#ifdef PUDDLEJUMPER_REX
        // Rex sessions are detached: they go on running on their server.
        if (RexSession *rex = rexSessionFor(tab)) {
            if (!rexSessions.contains(rex))
                rexSessions << rex;
            continue;
        }
#endif
        auto *terminal = qobject_cast<TerminalTab *>(tab);
        for (TerminalWidget *pane : terminal ? terminal->panes() : QList<TerminalWidget *>()) {
            if (pane->hasRunningProcess()) {
                const QString program = pane->foregroundProgramName();
                running << (program.isEmpty() ? tr("a program") : program);
            }
        }
    }
    if (!running.isEmpty() && m_settings.scrolling().confirmCloseWithRunningProcesses) {
        QMessageBox box(QMessageBox::Warning, tr("Close Workspace?"),
                        tr("Programs are still running in workspace “%1”.").arg(name),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Closing it will terminate:\n\n%1").arg(running.join(QLatin1Char('\n'))));
        QPushButton *close = box.addButton(tr("&Close Workspace"), QMessageBox::DestructiveRole);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != close)
            return;
    }

    // A workspace file run by tmux: keep it running (as closing any tmux
    // tab offers), or stop it as smug stop does.
    const Workspace &closing = m_workspaces.at(index);
    bool stopSessions = false;
    if (!closing.definition.isEmpty() && !sessions.isEmpty()) {
        QMessageBox box(QMessageBox::Question, tr("Close Workspace"),
                        tr("Workspace “%1” runs in tmux.").arg(name), QMessageBox::NoButton, this);
        box.setInformativeText(tr("Detach to keep it running, to open again later; or stop "
                                  "it, which runs its stop commands and ends the session."));
        QPushButton *detach = box.addButton(tr("&Detach"), QMessageBox::AcceptRole);
        QPushButton *stop = box.addButton(tr("&Stop Workspace"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(detach);
        box.exec();
        if (box.clickedButton() != detach && box.clickedButton() != stop)
            return;
        stopSessions = box.clickedButton() == stop;
    }

    // A workspace file's stop commands, before its programs go (not when
    // its tmux session is only being detached).
    if (!closing.definition.isEmpty() && (sessions.isEmpty() || stopSessions)) {
        QHash<QString, QString> variables = closing.variables;
        WorkspaceSpec spec;
        QString output;
        if (readWorkspaceFile(closing.definition, variables, spec, false) && !spec.stop.isEmpty()
            && !runSetupCommands(spec.stop, spec.root, spec.environment,
                                 tr("Stopping “%1”").arg(name), &output)) {
            QMessageBox box(QMessageBox::Warning, tr("Close Workspace"),
                            tr("A stop command for “%1” failed.").arg(name),
                            QMessageBox::NoButton, this);
            box.setDetailedText(output);
            QPushButton *close = box.addButton(tr("Close &Anyway"), QMessageBox::DestructiveRole);
            box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != close)
                return;
        }
    }

    // Off screen first, so its tabs are all parked.
    if (index == m_currentWorkspace)
        switchWorkspace(index == 0 ? 1 : index - 1);

    // tmux sessions are detached, unless stopped; their tabs go when they have.
    for (TmuxSession *session : std::as_const(sessions)) {
        if (stopSessions)
            session->killSession();
        else
            session->detach();
    }
#ifdef PUDDLEJUMPER_REX
    for (RexSession *rex : std::as_const(rexSessions))
        rex->detach();
#endif
    for (Tab *tab : std::as_const(tabs)) {
        bool sessionTab = tmuxSessionFor(tab) != nullptr;
#ifdef PUDDLEJUMPER_REX
        sessionTab = sessionTab || rexSessionFor(tab);
#endif
        if (!sessionTab)
            tab->deleteLater();
    }
    const QString id = m_workspaces.at(index).id;
    m_workspaces.removeAt(index);
    if (m_currentWorkspace > index)
        --m_currentWorkspace;
    statusBar()->showMessage(tr("Closed workspace “%1”").arg(name), 4000);
    updateWorkspaceChrome();
}

void MainWindow::moveTabToWorkspace(Tab *tab, int index)
{
    if (!tab || index < 0 || index >= m_workspaces.size())
        return;
    const int from = workspaceOf(tab);
    if (from < 0 || from == index)
        return;

    QList<Tab *> tabs{tab};
    if (TmuxSession *session = tmuxSessionFor(tab)) {
        tabs.clear();
        for (TerminalTab *t : session->tabs())
            tabs << t;
        m_sessionWorkspace.insert(session, m_workspaces.at(index).id);
    }
#ifdef PUDDLEJUMPER_REX
    if (RexSession *rex = rexSessionFor(tab)) {
        tabs.clear();
        for (TerminalTab *t : rex->tabs())
            tabs << t;
        m_rexWorkspace.insert(rex, m_workspaces.at(index).id);
    }
#endif
    for (Tab *t : std::as_const(tabs)) {
        const int i = m_tabs->indexOf(t);
        if (i >= 0)
            m_tabs->removeTab(i);
        else
            m_workspaces[from].parked.removeAll(t);
        if (index == m_currentWorkspace) {
            m_tabs->addTab(t, QString());
            syncTabText(t);
        } else {
            t->setParent(m_parking);
            m_workspaces[index].parked << t;
        }
    }

    if (m_tabs->count() == 0) {
        newTab();
    } else {
        syncTabTitles();
        updateStatus();
    }
    statusBar()->showMessage(tr("Moved to workspace “%1”").arg(m_workspaces.at(index).name), 4000);
}

void MainWindow::rebuildWorkspaceMenu()
{
    m_workspaceMenu->clear();
    const QColor outline = palette().color(QPalette::WindowText);
    auto *group = new QActionGroup(m_workspaceMenu);
    for (int i = 0; i < m_workspaces.size(); ++i) {
        const Workspace &w = m_workspaces.at(i);
        // A dot after the name: something happened there while away.
        QAction *act = m_workspaceMenu->addAction(
            w.icon(outline), w.unseenOutput ? tr("%1  •").arg(w.name) : w.name);
        act->setCheckable(true);
        act->setChecked(i == m_currentWorkspace);
        act->setActionGroup(group);
        if (!w.root.isEmpty())
            act->setToolTip(tildePath(w.root));
        // By id, decided when chosen: the menu may be a stale copy.
        const QString id = w.id;
        connect(act, &QAction::triggered, this, [this, id]() { switchWorkspace(workspaceIndex(id)); });
    }
    m_workspaceMenu->addSeparator();
    m_workspaceMenu->addAction(m_newWorkspaceAct);

    // Workspace files: puddlejumper's, then smug's; those open are ticked.
    m_openWorkspaceMenu->clear();
    for (const auto &[name, path] : WorkspaceFile::available()) {
        QAction *act = m_openWorkspaceMenu->addAction(name);
        act->setToolTip(tildePath(path));
        bool open = false;
        for (const Workspace &w : std::as_const(m_workspaces))
            open = open || w.definition == path;
        act->setCheckable(open);
        act->setChecked(open);
        const QString file = path;
        connect(act, &QAction::triggered, this, [this, file]() { openWorkspaceFile(file); });
    }
    if (m_openWorkspaceMenu->isEmpty()) {
        m_openWorkspaceMenu->addAction(tr("No workspace files yet"))->setEnabled(false);
        m_openWorkspaceMenu->actions().constLast()->setToolTip(
            WorkspaceFile::directories().join(QLatin1Char('\n')));
    }
    m_openWorkspaceMenu->addSeparator();
    m_openWorkspaceMenu->addAction(tr("Open &File…"), this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Open Workspace File"), QDir::homePath(),
            tr("Workspace files (*.yml *.yaml);;All files (*)"));
        if (!path.isEmpty())
            openWorkspaceFile(path);
    });
    m_workspaceMenu->addMenu(m_openWorkspaceMenu);
    m_workspaceMenu->addAction(appIcon("folder-open"),
                               tr("Open &Folder…"), this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Folder"),
                                                              QDir::homePath());
        if (!dir.isEmpty())
            openFolder(dir);
    });

    // Windows the current workspace's file keeps for when they're asked for.
    const Workspace &current = m_workspaces.at(m_currentWorkspace);
    m_startWindowMenu->clear();
    if (!current.definition.isEmpty()) {
        QHash<QString, QString> variables = current.variables;
        WorkspaceSpec spec;
        if (readWorkspaceFile(current.definition, variables, spec, false)) {
            for (const WorkspaceSpec::Window &win : std::as_const(spec.windows)) {
                if (!win.manual)
                    continue;
                bool running = false;
                for (int i = 0; i < m_tabs->count(); ++i) {
                    TerminalTab *tab = tabAt(i);
                    TmuxSession *session = tmuxSessionFor(tab);
                    running = running || (tab && tab->customTitle() == win.name)
                              || (session && session->windowName(tab) == win.name);
                }
                QAction *act = m_startWindowMenu->addAction(win.name);
                act->setEnabled(!running);
                const QString name = win.name;
                connect(act, &QAction::triggered, this, [this, name]() { startManualWindow(name); });
            }
        }
    }
    if (!m_startWindowMenu->isEmpty())
        m_workspaceMenu->addMenu(m_startWindowMenu);

    m_workspaceMenu->addAction(m_saveWorkspaceAct);
    m_workspaceMenu->addSeparator();
    m_workspaceMenu->addAction(m_editWorkspaceAct);
    m_editWorkspaceFileAct->setEnabled(!current.definition.isEmpty());
    m_workspaceMenu->addAction(m_editWorkspaceFileAct);
    m_closeWorkspaceAct->setEnabled(m_workspaces.size() > 1);
    m_workspaceMenu->addAction(m_closeWorkspaceAct);
    m_workspaceMenu->addSeparator();
    m_nextWorkspaceAct->setEnabled(m_workspaces.size() > 1);
    m_prevWorkspaceAct->setEnabled(m_workspaces.size() > 1);
    m_workspaceMenu->addAction(m_nextWorkspaceAct);
    m_workspaceMenu->addAction(m_prevWorkspaceAct);
}

void MainWindow::updateWorkspaceChrome()
{
    const Workspace &w = m_workspaces.at(m_currentWorkspace);
    const QIcon icon = w.icon(palette().color(QPalette::WindowText));
    const bool unseenElsewhere = std::any_of(m_workspaces.cbegin(), m_workspaces.cend(),
                                             [](const Workspace &ws) { return ws.unseenOutput; });
    const QString text = unseenElsewhere ? tr("%1  •").arg(w.name) : w.name;
    if (m_workspaceButton) {
        m_workspaceButton->setText(text);
        m_workspaceButton->setIcon(icon);
    }
    // The sidebar has the terminal's colours, so its dot is outlined in those.
    m_sidebar->setWorkspace(text, w.icon(m_sidebar->palette().color(QPalette::WindowText)),
                            m_workspaceMenu, m_workspaces.size() > 1);
    if (m_workspaceMenu)
        rebuildWorkspaceMenu();   // a global menu shows a copy: keep it current
}

void MainWindow::markTabBusy(Tab *tab)
{
    if (!tab || QDateTime::currentMSecsSinceEpoch() < tab->property(kQuietUntilProperty).toLongLong())
        return;
    const int i = m_tabs->indexOf(tab);
    if (i >= 0) {
        if (i != m_tabs->currentIndex()) {
            tab->setProperty(kBusyProperty, true);
            m_sidebar->setBusy(i, true);
            m_tabStrip->setBusy(i, true);
        }
        return;
    }
    tab->setProperty(kBusyProperty, true);
    const int w = workspaceOf(tab);
    if (w >= 0 && !m_workspaces.at(w).unseenOutput) {
        m_workspaces[w].unseenOutput = true;
        updateWorkspaceChrome();
    }
}

void MainWindow::saveSession()
{
    // Local tabs as layouts of directories and profiles; tmux sessions on
    // this machine by how they were attached (over ssh, they are left be).
    const auto describe = [](TerminalWidget *pane) {
        return QJsonObject{
            {QStringLiteral("dir"), pane->currentDirectory()},
            {QStringLiteral("profile"), pane->profileId()},
        };
    };
    QJsonArray workspaces;
    for (int i = 0; i < m_workspaces.size(); ++i) {
        const Workspace &w = m_workspaces.at(i);
        QList<Tab *> tabs;
        int currentTab = w.currentTab;
        if (i == m_currentWorkspace) {
            for (int t = 0; t < m_tabs->count(); ++t)
                tabs << pageAt(t);
            currentTab = m_tabs->currentIndex();
        } else {
            for (const QPointer<Tab> &t : w.parked)
                tabs << t.data();
        }

        QJsonArray savedTabs;
        int savedCurrent = 0;
        for (int t = 0; t < tabs.size(); ++t) {
            Tab *page = tabs.at(t);
            if (!page || tmuxSessionFor(page))
                continue;
#ifdef PUDDLEJUMPER_REX
            if (rexSessionFor(page))
                continue;   // saved by session below
#endif
            if (t == currentTab)
                savedCurrent = savedTabs.size();
#ifdef PUDDLEJUMPER_WEB
            if (auto *web = qobject_cast<WebTab *>(page)) {
                QJsonObject saved{
                    {QStringLiteral("title"), web->customTitle()},
                    {QStringLiteral("web"), web->url().toString()},
                };
                if (web->reloadAfterCommands())
                    saved.insert(QStringLiteral("reloadAfterCommands"), true);
                savedTabs.append(saved);
                continue;
            }
#endif
            if (auto *vnc = qobject_cast<VncTab *>(page)) {
                // Under its protocol: "vnc" or "rdp".
                QJsonObject saved{
                    {QStringLiteral("title"), vnc->customTitle()},
                    {vnc->view()->client()->scheme(), vnc->address()},
                };
                if (!vnc->username().isEmpty())
                    saved.insert(QStringLiteral("user"), vnc->username());
                if (vnc->reducedColour())
                    saved.insert(QStringLiteral("reducedColour"), true);
                if (vnc->adaptiveQuality())
                    saved.insert(QStringLiteral("adaptive"), true);
                savedTabs.append(saved);
                continue;
            }
            auto *tab = qobject_cast<TerminalTab *>(page);
            if (!tab)
                continue;
            savedTabs.append(QJsonObject{
                {QStringLiteral("title"), tab->customTitle()},
                {QStringLiteral("layout"), tab->saveLayout(describe)},
            });
        }
        QJsonArray tmux;
        for (TmuxSession *session : std::as_const(m_tmuxSessions)) {
            const QStringList command = session->command();
            if (m_sessionWorkspace.value(session) != w.id || command.isEmpty()
                || command.first() == QLatin1String("ssh"))
                continue;
            tmux.append(QJsonObject{
                {QStringLiteral("label"), session->label()},
                {QStringLiteral("command"), QJsonArray::fromStringList(command)},
            });
        }
        // Rex sessions by server and id, to attach to again if they're
        // still there.
        QJsonArray rex;
#ifdef PUDDLEJUMPER_REX
        for (RexSession *session : std::as_const(m_rexSessions)) {
            if (m_rexWorkspace.value(session) != w.id || !session->isAttached())
                continue;
            rex.append(QJsonObject{
                {QStringLiteral("server"), session->server().toString()},
                {QStringLiteral("session"), session->sessionId()},
            });
        }
#endif
        QJsonObject variables;
        for (auto it = w.variables.cbegin(); it != w.variables.cend(); ++it)
            variables.insert(it.key(), it.value());
        workspaces.append(QJsonObject{
            {QStringLiteral("id"), w.id},
            {QStringLiteral("name"), w.name},
            {QStringLiteral("root"), w.root},
            {QStringLiteral("colour"), w.colour.isValid() ? w.colour.name() : QString()},
            {QStringLiteral("definition"), w.definition},
            {QStringLiteral("variables"), variables},
            {QStringLiteral("currentTab"), savedCurrent},
            {QStringLiteral("tabs"), savedTabs},
            {QStringLiteral("tmux"), tmux},
            {QStringLiteral("rex"), rex},
        });
    }
    const QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("current"), m_currentWorkspace},
        {QStringLiteral("workspaces"), workspaces},
    };
    QSettings().setValue(QLatin1String(kSessionKey),
                         QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)));
}

bool MainWindow::restoreSession()
{
    const QJsonObject state = QJsonDocument::fromJson(
        QSettings().value(QLatin1String(kSessionKey)).toString().toUtf8()).object();
    const QJsonArray saved = state.value(QStringLiteral("workspaces")).toArray();
    if (saved.isEmpty())
        return false;

    QList<Workspace> workspaces;
    for (const QJsonValue &v : saved) {
        const QJsonObject o = v.toObject();
        Workspace w;
        w.id = o.value(QStringLiteral("id")).toString();
        if (w.id.isEmpty())
            w.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        w.name = o.value(QStringLiteral("name")).toString();
        if (w.name.isEmpty())
            w.name = tr("Workspace");
        w.root = o.value(QStringLiteral("root")).toString();
        w.colour = QColor(o.value(QStringLiteral("colour")).toString());
        w.currentTab = o.value(QStringLiteral("currentTab")).toInt();
        w.definition = o.value(QStringLiteral("definition")).toString();
        const QJsonObject variables = o.value(QStringLiteral("variables")).toObject();
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it)
            w.variables.insert(it.key(), it.value().toString());
        workspaces << w;
    }
    m_workspaces = workspaces;
    m_currentWorkspace = qBound(0, state.value(QStringLiteral("current")).toInt(),
                                int(m_workspaces.size()) - 1);

    // Sessions on this machine still running, to reattach to.
    QStringList running;
    {
        QProcess list;
        const QStringList program = tmuxProgram();
        list.start(program.first(), program.mid(1) + QStringList{
            QStringLiteral("list-sessions"), QStringLiteral("-F"), QStringLiteral("#{session_name}")});
        if (list.waitForFinished(2000))
            running = QString::fromUtf8(list.readAllStandardOutput()).split(QLatin1Char('\n'));
    }

    for (int i = 0; i < saved.size(); ++i) {
        const QJsonObject o = saved.at(i).toObject();
        const QString root = m_workspaces.at(i).root;
        const auto makeSavedPane = [this, &root](const QJsonObject &p) {
            // Where it was; failing that the workspace's folder, or the profile's.
            QString dir = p.value(QStringLiteral("dir")).toString();
            if (dir.isEmpty() || !QDir(dir).exists())
                dir = !root.isEmpty() && QDir(root).exists() ? root : QString();
            return makePane(m_profiles.profileById(p.value(QStringLiteral("profile")).toString()), dir);
        };
        for (const QJsonValue &t : o.value(QStringLiteral("tabs")).toArray()) {
            const QJsonObject savedTab = t.toObject();
            if (const QString address = savedTab.value(QStringLiteral("web")).toString();
                !address.isEmpty()) {
#ifdef PUDDLEJUMPER_WEB
                auto *web = new WebTab(QUrl(address));
                web->setCustomTitle(savedTab.value(QStringLiteral("title")).toString());
                web->setReloadAfterCommands(savedTab.value(QStringLiteral("reloadAfterCommands")).toBool());
                addTab(web, web->title(), QString(), i);
                if (i == m_currentWorkspace)
                    syncTabText(web);
#endif
                continue;
            }
            if (const QString address = savedTab.value(QStringLiteral("vnc")).toString();
                !address.isEmpty()) {
                QString host;
                quint16 port;
                if (!VncView::parseAddress(address, &host, &port))
                    continue;
                auto *vnc = new VncTab(host, port,
                                       savedTab.value(QStringLiteral("reducedColour")).toBool());
                // Still connecting: its setup picks this up.
                vnc->setAdaptiveQuality(savedTab.value(QStringLiteral("adaptive")).toBool());
                vnc->setCustomTitle(savedTab.value(QStringLiteral("title")).toString());
                vnc->setUsername(savedTab.value(QStringLiteral("user")).toString());
                addTab(vnc, address, QString(), i);
                if (i == m_currentWorkspace)
                    syncTabText(vnc);
                continue;
            }
#ifdef PUDDLEJUMPER_RDP
            if (const QString address = savedTab.value(QStringLiteral("rdp")).toString();
                !address.isEmpty()) {
                QString host;
                quint16 port;
                if (!RdpClient::parseAddress(address, &host, &port))
                    continue;
                auto *rdp = new VncTab(host, port, false, nullptr, new RdpClient);
                rdp->setCustomTitle(savedTab.value(QStringLiteral("title")).toString());
                rdp->setUsername(savedTab.value(QStringLiteral("user")).toString());
                addTab(rdp, address, QString(), i);
                if (i == m_currentWorkspace)
                    syncTabText(rdp);
                continue;
            }
#endif
            TerminalTab *tab = TerminalTab::restoreLayout(
                savedTab.value(QStringLiteral("layout")).toObject(), makeSavedPane);
            if (!tab)
                continue;
            tab->setCustomTitle(savedTab.value(QStringLiteral("title")).toString());
            addTab(tab, tr("Terminal"), QString(), i);
            if (i == m_currentWorkspace)
                syncTabText(tab);
        }
        for (const QJsonValue &t : o.value(QStringLiteral("tmux")).toArray()) {
            const QJsonObject session = t.toObject();
            const QString label = session.value(QStringLiteral("label")).toString();
            QStringList command;
            for (const QJsonValue &arg : session.value(QStringLiteral("command")).toArray())
                command << arg.toString();
            if (!label.isEmpty() && !command.isEmpty() && running.contains(label))
                attachTmux(label, command, m_workspaces.at(i).id);
        }
#ifdef PUDDLEJUMPER_REX
        for (const QJsonValue &t : o.value(QStringLiteral("rex")).toArray()) {
            const QJsonObject session = t.toObject();
            const QUrl server(session.value(QStringLiteral("server")).toString());
            const QString id = session.value(QStringLiteral("session")).toString();
            if (server.isValid() && !id.isEmpty())
                openRex(server, id, m_workspaces.at(i).id, true);
        }
#endif
    }

    if (m_tabs->count() == 0) {
        newTab();
    } else {
        const int current = qBound(0, m_workspaces.at(m_currentWorkspace).currentTab,
                                   m_tabs->count() - 1);
        m_tabs->setCurrentIndex(current);
        onCurrentTabChanged(current);
    }
    return true;
}

// ---- workspace files ----------------------------------------------------------

bool MainWindow::readWorkspaceFile(const QString &path, QHash<QString, QString> &variables,
                                   WorkspaceSpec &spec, bool askForVariables)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (askForVariables)
            QMessageBox::warning(this, tr("Open Workspace"),
                                 tr("Couldn't read %1: %2").arg(tildePath(path), file.errorString()));
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());

    // Variables neither given nor in the environment: ask for them (smug
    // takes them on its command line).
    const QStringList missing = WorkspaceFile::missingVariables(text, variables);
    if (!missing.isEmpty()) {
        if (!askForVariables) {
            for (const QString &name : missing)
                variables.insert(name, QString());
        } else {
            QDialog dialog(this);
            dialog.setWindowTitle(tr("Open Workspace"));
            auto *form = new QFormLayout(&dialog);
            auto *intro = new QLabel(tr("“%1” uses variables that aren't set:")
                                         .arg(QFileInfo(path).completeBaseName()), &dialog);
            form->addRow(intro);
            QList<QLineEdit *> edits;
            for (const QString &name : missing) {
                auto *edit = new QLineEdit(&dialog);
                form->addRow(name, edit);
                edits << edit;
            }
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            buttons->button(QDialogButtonBox::Ok)->setText(tr("&Open"));
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            form->addRow(buttons);
            if (dialog.exec() != QDialog::Accepted)
                return false;
            for (int i = 0; i < missing.size(); ++i)
                variables.insert(missing.at(i), edits.at(i)->text());
        }
    }

    QString error;
    if (!WorkspaceFile::parse(WorkspaceFile::expand(text, variables), path, spec, &error)) {
        if (askForVariables)
            QMessageBox::warning(this, tr("Open Workspace"),
                                 tr("%1 isn't a workspace file puddlejumper can read.\n\n%2")
                                     .arg(tildePath(path), error));
        return false;
    }
    return true;
}

void MainWindow::openWorkspaceFile(const QString &path)
{
    const QString file = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_workspaces.size(); ++i) {
        if (m_workspaces.at(i).definition == file) {
            switchWorkspace(i);
            return;
        }
    }

    QHash<QString, QString> variables;
    WorkspaceSpec spec;
    if (!readWorkspaceFile(file, variables, spec, true))
        return;

    // A file from elsewhere (a project, a download) runs commands: ask once,
    // and again whenever it changes. The user's own files are trusted.
    if (!WorkspaceFile::isUsersOwn(file)) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            return;
        const QByteArray hash = QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
        const QString key = QStringLiteral("trustedWorkspaceFiles/")
                            + QString::fromLatin1(QCryptographicHash::hash(file.toUtf8(),
                                                  QCryptographicHash::Sha1).toHex());
        QSettings settings;
        const QStringList commands = WorkspaceFile::commands(spec);
        if (!commands.isEmpty() && settings.value(key).toByteArray() != hash) {
            QMessageBox box(QMessageBox::Warning, tr("Open Workspace"),
                            tr("Run the commands in “%1”?").arg(tildePath(file)),
                            QMessageBox::NoButton, this);
            box.setInformativeText(tr("Opening this workspace runs %n command(s). Only open "
                                      "files you trust; you'll be asked again if it changes.",
                                      nullptr, int(commands.size())));
            box.setDetailedText(commands.join(QLatin1Char('\n')));
            QPushButton *trust = box.addButton(tr("&Trust and Open"), QMessageBox::AcceptRole);
            QPushButton *cancel = box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(cancel);
            box.exec();
            if (box.clickedButton() != trust)
                return;
            settings.setValue(key, hash);
        }
    }

    // Run by tmux: an existing session is attached to as it is, as smug does;
    // otherwise it's made, then attached to.
    const QStringList tmux = tmuxProgram();
    const bool tmuxRunning = spec.tmux && TmuxWorkspace::sessionExists(tmux, spec.name);
    if (tmuxRunning) {
        for (TmuxSession *s : std::as_const(m_tmuxSessions)) {
            if (s->label() == spec.name && !s->tabs().isEmpty()) {
                showTab(s->tabs().first());   // already open, in another workspace
                return;
            }
        }
    }

    QString output;
    if (!tmuxRunning && !spec.beforeStart.isEmpty()
        && !runSetupCommands(spec.beforeStart, spec.root, spec.environment,
                             tr("Starting “%1”").arg(spec.name), &output)) {
        QMessageBox box(QMessageBox::Warning, tr("Open Workspace"),
                        tr("“%1” wasn't opened: a before_start command failed.").arg(spec.name),
                        QMessageBox::Ok, this);
        box.setDetailedText(output);
        box.exec();
        return;
    }

    Workspace ws;
    ws.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ws.name = spec.name;
    ws.root = spec.root;
    ws.colour = spec.colour;
    ws.definition = file;
    ws.variables = variables;

    if (spec.tmux) {
        QString error;
        // The size the session will be shown at, in cells, as TmuxSession
        // works it out: the page less its margin and a pane's frame.
        QSize cells;
        if (TerminalWidget *pane = currentTerminal(); pane && m_tabs->currentWidget()) {
            const QSize cell = pane->cellSize();
            const int m = m_settings.appearance().terminalMargin;
            const QSize area = m_tabs->currentWidget()->size() - QSize(2 * m, 2 * m)
                               - TerminalWidget::frameChrome(true);
            if (!cell.isEmpty())
                cells = QSize(qMax(10, area.width() / cell.width()), qMax(3, area.height() / cell.height()));
        }
        if (!tmuxRunning && !TmuxWorkspace::createSession(tmux, spec, cells, &error)) {
            QMessageBox::warning(this, tr("Open Workspace"),
                                 tr("Couldn't start tmux session “%1”.\n\n%2").arg(spec.name, error));
            return;
        }
        m_workspaces << ws;
        updateWorkspaceChrome();
        attachTmux(spec.name, tmux + QStringList{QStringLiteral("-u"), QStringLiteral("-C"),
                                                 QStringLiteral("attach-session"), QStringLiteral("-t"),
                                                 spec.name},
                   ws.id, true);
        return;
    }

    m_workspaces << ws;
    const int index = m_workspaces.size() - 1;

    int selected = -1;
    int opened = 0;
    for (const WorkspaceSpec::Window &win : std::as_const(spec.windows)) {
        if (win.manual)
            continue;
        TerminalTab *tab = makeWindowTab(spec, win);
        if (!tab)
            continue;
        if (win.selected)
            selected = opened;
        ++opened;
        addTab(tab, win.name.isEmpty() ? tr("Terminal") : win.name, QString(), index);
    }
    switchWorkspace(index);
    if (selected >= 0)
        m_tabs->setCurrentIndex(selected);
}

TerminalTab *MainWindow::makeWindowTab(const WorkspaceSpec &spec, const WorkspaceSpec::Window &window)
{
    TerminalTab *tab = TerminalTab::restoreLayout(window.layout, [this, &spec](const QJsonObject &leaf) {
        TerminalProfile profile = m_profiles.defaultProfile();
        const QString wanted = leaf.value(QStringLiteral("profile")).toString();
        for (const TerminalProfile &p : m_profiles.profiles()) {
            if (!wanted.isEmpty() && (p.name == wanted || p.id == wanted))
                profile = p;
        }
        QStringList commands;
        for (const QJsonValue &c : leaf.value(QStringLiteral("commands")).toArray())
            commands << c.toString();
        // exec: the commands are the pane's program, and it closes with them.
        const bool exec = leaf.value(QStringLiteral("exec")).toBool() && !commands.isEmpty();
        if (exec) {
            const QStringList shell = scriptCommand(commands.join(QLatin1Char('\n')));
            profile.shellProgram = shell.first();
            profile.shellArgs = shell.mid(1);
        }
        profile.environment = spec.environment;

        QString dir = leaf.value(QStringLiteral("dir")).toString();
        if (dir.isEmpty() || !QDir(dir).exists())
            dir = !spec.root.isEmpty() && QDir(spec.root).exists() ? spec.root : QString();
        TerminalWidget *pane = makePane(profile, dir);

        // Typed into the shell, as smug does, so it's still there afterwards
        // and they're in its history.
        if (!exec) {
            const int step = qMax(spec.sendKeysDelay, 30);
            for (int i = 0; i < commands.size(); ++i) {
                const QString command = commands.at(i);
                QTimer::singleShot(100 + i * step, pane, [pane, command]() {
                    pane->sendText(command + QLatin1Char('\r'));
                });
            }
        }
        return pane;
    });
    if (tab)
        tab->setCustomTitle(window.name);
    return tab;
}

void MainWindow::startManualWindow(const QString &name)
{
    Workspace &current = m_workspaces[m_currentWorkspace];
    WorkspaceSpec spec;
    if (current.definition.isEmpty()
        || !readWorkspaceFile(current.definition, current.variables, spec, true))
        return;
    for (const WorkspaceSpec::Window &win : std::as_const(spec.windows)) {
        if (win.name != name)
            continue;
        // In the workspace's tmux session, if it has one: the tab follows.
        if (spec.tmux) {
            for (TmuxSession *s : std::as_const(m_tmuxSessions)) {
                if (s->label() != spec.name)
                    continue;
                QString error;
                if (!TmuxWorkspace::addWindow(tmuxProgram(), spec, win, &error))
                    QMessageBox::warning(this, tr("Start Window"), error);
                return;
            }
        }
        if (TerminalTab *tab = makeWindowTab(spec, win)) {
            const int index = addTab(tab, name, QString());
            m_tabs->setCurrentIndex(index);
        }
        return;
    }
}

void MainWindow::saveWorkspaceAs()
{
    Workspace &w = m_workspaces[m_currentWorkspace];

    // Each tab as a window: its panes' directories, and what each is running
    // as the command to start it with. A tmux pane's come from tmux and, for
    // a server on this machine, the process table.
    const auto describe = [this](TerminalWidget *pane) {
        QString dir = pane->currentDirectory();
        QStringList args = pane->foregroundCommandLine();
        if (TmuxSession *session = tmuxSessionFor(pane);
            session && session->command().value(0) != QLatin1String("ssh")) {
            QProcess p;
            const QStringList tmux = tmuxProgram();
            p.start(tmux.first(), tmux.mid(1) + QStringList{
                QStringLiteral("display-message"), QStringLiteral("-p"), QStringLiteral("-t"),
                QLatin1Char('%') + QString::number(session->tmuxPaneId(pane)),
                QStringLiteral("#{pane_pid}\t#{pane_current_path}")});
            p.waitForFinished(2000);
            const QStringList f = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed()
                                      .split(QLatin1Char('\t'));
            dir = f.value(1);
            // The pane's foreground process group, from its shell's terminal.
            const int shellPid = f.value(0).toInt();
            const int foreground = procinfo::terminalForegroundGroup(shellPid);
            if (foreground > 0 && foreground != shellPid)
                args << procinfo::arguments(foreground);
        }
        QJsonObject o{{QStringLiteral("dir"), dir}};
        if (!args.isEmpty()) {
            QStringList quoted;
            for (const QString &arg : args) {
                static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./:=@%+-]+$"));
                quoted << (plain.match(arg).hasMatch() ? arg : shellQuote(arg));
            }
            o.insert(QStringLiteral("commands"), QJsonArray{quoted.join(QLatin1Char(' '))});
        }
        return o;
    };
    QList<WorkspaceFile::SavedWindow> windows;
    bool tmux = false;
    for (int i = 0; i < m_tabs->count(); ++i) {
        TerminalTab *tab = tabAt(i);
        if (!tab)
            continue;
        QString name = tab->customTitle();
        if (TmuxSession *session = tmuxSessionFor(tab)) {
            name = session->windowName(tab);
            tmux = true;
        }
        if (name.isEmpty() && tab->activePane()) {
            name = tab->activePane()->hasRunningProcess()
                       ? tab->activePane()->foregroundProgramName()
                       : QDir(tab->activePane()->currentDirectory()).dirName();
        }
        windows << WorkspaceFile::SavedWindow{name, tab->saveLayout(describe)};
    }

    // Shown for review before it's written: the commands are a guess.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Save Workspace As"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *pathRow = new QHBoxLayout;
    auto *pathEdit = new QLineEdit(&dialog);
    QString suggested = w.definition;
    if (suggested.isEmpty()) {
        QString base = w.name.toLower();
        base.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("-"));
        suggested = WorkspaceFile::directories().first() + QLatin1Char('/')
                    + (base.isEmpty() ? QStringLiteral("workspace") : base) + QStringLiteral(".yml");
    }
    pathEdit->setText(suggested);
    auto *browse = new QToolButton(&dialog);
    browse->setIcon(appIcon("folder-open"));
    connect(browse, &QToolButton::clicked, &dialog, [&dialog, pathEdit]() {
        const QString p = QFileDialog::getSaveFileName(&dialog, tr("Save Workspace As"), pathEdit->text(),
                                                       tr("Workspace files (*.yml *.yaml)"));
        if (!p.isEmpty())
            pathEdit->setText(p);
    });
    pathRow->addWidget(new QLabel(tr("File:"), &dialog));
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);
    auto *hint = new QLabel(tr("Each pane's command is what it's running now. Check and edit "
                               "before saving; files in %1 appear under Open Workspace.")
                                .arg(tildePath(WorkspaceFile::directories().first())), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *editor = new QPlainTextEdit(&dialog);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    QString original;
    if (QFile file(w.definition); !w.definition.isEmpty() && file.open(QIODevice::ReadOnly))
        original = QString::fromUtf8(file.readAll());
    editor->setPlainText(WorkspaceFile::write(w.name, w.root, w.colour, windows, original, tmux));
    editor->setMinimumSize(560, 360);
    layout->addWidget(editor, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString path = pathEdit->text().trimmed();
    if (path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(editor->toPlainText().toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Save Workspace As"),
                             tr("Couldn't write %1: %2").arg(tildePath(path), file.errorString()));
        return;
    }
    w.definition = QFileInfo(path).absoluteFilePath();
    statusBar()->showMessage(tr("Saved workspace “%1” to %2").arg(w.name, tildePath(path)), 5000);
    updateWorkspaceChrome();
}

void MainWindow::editWorkspaceFile()
{
    const QString path = m_workspaces.at(m_currentWorkspace).definition;
    if (path.isEmpty())
        return;
#ifdef Q_OS_WIN
    // No $EDITOR there, nor one shell to ask for it.
    QProcess::startDetached(QStringLiteral("notepad.exe"), {QDir::toNativeSeparators(path)});
    return;
#endif
    // In the terminal's own editor, in a tab of its own.
    newTabWithProfile(QString());
    if (TerminalWidget *pane = currentTerminal()) {
        pane->sendText(QStringLiteral("${VISUAL:-${EDITOR:-nano}} %1\r").arg(shellQuote(path)));
        if (TerminalTab *tab = currentTab()) {
            tab->setCustomTitle(QFileInfo(path).fileName());
            syncTabText(tab);
        }
    }
}

bool MainWindow::runSetupCommands(const QStringList &commands, const QString &dir,
                                  const QStringList &environment, const QString &what,
                                  QString *output)
{
    if (!dir.isEmpty() && !QDir(dir).exists()) {
        *output = tr("The folder %1 doesn't exist.").arg(dir);
        return false;
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &pair : environment) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq > 0)
            env.insert(pair.left(eq), pair.mid(eq + 1));
    }

    QProgressDialog progress(what, tr("Cancel"), 0, int(commands.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    for (int i = 0; i < commands.size(); ++i) {
        const QString command = commands.at(i);
        progress.setLabelText(QStringLiteral("%1\n%2").arg(what, command));
        progress.setValue(i);

        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.setWorkingDirectory(dir.isEmpty() ? QDir::homePath() : dir);
        proc.setProcessChannelMode(QProcess::MergedChannels);
        QEventLoop loop;
        connect(&proc, &QProcess::finished, &loop, &QEventLoop::quit);
        connect(&proc, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
        const QStringList shell = scriptCommand(command);
        proc.start(shell.first(), shell.mid(1));
        if (proc.state() != QProcess::NotRunning)
            loop.exec();

        *output += QStringLiteral("$ %1\n").arg(command) + QString::fromLocal8Bit(proc.readAll());
        if (progress.wasCanceled()) {
            proc.kill();
            proc.waitForFinished(1000);
            *output += tr("(cancelled)\n");
            return false;
        }
        if (proc.state() != QProcess::NotRunning)
            proc.waitForFinished(-1);
        if (proc.error() == QProcess::FailedToStart || proc.exitStatus() != QProcess::NormalExit
            || proc.exitCode() != 0) {
            *output += tr("(exit status %1)\n").arg(proc.exitCode());
            return false;
        }
    }
    progress.setValue(int(commands.size()));
    return true;
}

// ---- project folders ----------------------------------------------------------

void MainWindow::openFolder(const QString &dir)
{
    const QString folder = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    const QString file = WorkspaceFile::projectFileIn(folder);
    if (!file.isEmpty()) {
        openWorkspaceFile(file);
        return;
    }
    // No file: a workspace for the folder, named after it.
    for (int i = 0; i < m_workspaces.size(); ++i) {
        if (QDir::cleanPath(m_workspaces.at(i).root) == folder && m_workspaces.at(i).definition.isEmpty()) {
            switchWorkspace(i);
            return;
        }
    }
    Workspace ws;
    ws.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ws.name = QDir(folder).dirName();
    ws.root = folder;
    m_workspaces << ws;
    switchWorkspace(m_workspaces.size() - 1);
}

void MainWindow::openWorkspaceTarget(const QString &target)
{
    const QFileInfo info(target);
    if (info.isDir()) {
        openFolder(info.absoluteFilePath());
        return;
    }
    if (info.isFile()) {
        openWorkspaceFile(info.absoluteFilePath());
        return;
    }
    for (const auto &[name, path] : WorkspaceFile::available()) {
        if (name == target) {
            openWorkspaceFile(path);
            return;
        }
    }
    QMessageBox::warning(this, tr("Open Workspace"),
                         tr("There's no folder, file or workspace called “%1”.\n\n"
                            "Named workspaces are the files in %2.")
                             .arg(target, WorkspaceFile::directories().join(tr(" and "))));
}

void MainWindow::updateProjectNotice()
{
    // Looked for only when the shell moves; whether it's open, every time.
    TerminalWidget *pane = currentTerminal();
    const QString dir = pane ? pane->currentDirectory() : QString();
    if (dir != m_projectNoticeDir) {
        m_projectNoticeDir = dir;
        m_projectNoticeFile = WorkspaceFile::findProjectFile(dir);
    }
    bool open = false;
    for (const Workspace &w : std::as_const(m_workspaces))
        open = open || w.definition == m_projectNoticeFile;
    if (m_projectNoticeFile.isEmpty() || open) {
        m_projectNotice->hide();
        return;
    }
    const QString text = tr("%1 has a workspace file · <a href=\"open\">Open</a>")
                             .arg(QDir(QFileInfo(m_projectNoticeFile).absolutePath()).dirName()
                                      .toHtmlEscaped());
    if (m_projectNotice->text() != text) {
        m_projectNotice->setText(text);
        m_projectNotice->setToolTip(tildePath(m_projectNoticeFile));
    }
    m_projectNotice->show();
}
