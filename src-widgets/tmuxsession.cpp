/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "tmuxsession.h"

#include "settings.h"
#include "terminalwidget.h"
#include "tmuxcontrol.h"

#include <QRegularExpression>
#include <QTimer>

namespace {

// TerminalWidget ids for tmux panes, clear of local ones.
constexpr int kPaneIdBase = 100000;

// Cap on output kept for a pane that doesn't exist yet. It only has to bridge
// the gap until its window's layout arrives, which is a round trip.
constexpr int kEarlyOutputLimit = 1 << 20;

// Bytes per send-keys command. Each byte is three characters of hex, and a
// short command line keeps one keystroke's latency low.
constexpr int kSendChunk = 128;

/*
 * tmux targets: "%3" for pane 3, "@2" for window 2. Built by concatenation,
 * never with QString::arg(): substituting "%3" into a template creates a new
 * "%3" placeholder that the next arg() call would replace.
 */
QString paneTarget(int id) { return QLatin1Char('%') + QString::number(id); }
QString windowTarget(int id) { return QLatin1Char('@') + QString::number(id); }

int parseId(const QByteArray &token)
{
    bool ok = false;
    const int id = token.mid(1).toInt(&ok);
    return ok ? id : -1;
}

/* Lines of a capture as one stream for the emulator. */
QByteArray joinCapture(const QList<QByteArray> &lines)
{
    QByteArray out;
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += "\r\n";
        out += lines.at(i);
    }
    return out;
}

QByteArray cursorTo(int x, int y)
{
    return "\033[0m\033[" + QByteArray::number(y + 1) + ';' + QByteArray::number(x + 1) + 'H';
}

void collectPaneIds(const PaneLayout &node, QSet<int> &out)
{
    if (node.isLeaf())
        out.insert(node.paneId);
    for (const PaneLayout &child : node.children)
        collectPaneIds(child, out);
}

} // namespace

TmuxSession::TmuxSession(const QString &label, const QStringList &command,
                         const Settings &settings, const TerminalProfile &profile,
                         QObject *parent)
    : QObject(parent)
    , m_label(label)
    , m_command(command)
    , m_settings(settings)
    , m_profile(profile)
{
    m_control = new TmuxControl(this);

    m_sizeTimer = new QTimer(this);
    m_sizeTimer->setSingleShot(true);
    m_sizeTimer->setInterval(50);
    connect(m_sizeTimer, &QTimer::timeout, this, &TmuxSession::sendClientSize);

    // A splitter drag sends one resize when it pauses, not one per pixel: each
    // resize comes back as a layout, which would fight the drag in progress.
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(150);
    connect(m_resizeTimer, &QTimer::timeout, this, [this]() {
        if (m_resizePane < 0)
            return;
        m_control->command(QStringLiteral("resize-pane -t ") + paneTarget(m_resizePane)
                           + (m_resizeOrientation == Qt::Horizontal ? QStringLiteral(" -x ")
                                                                    : QStringLiteral(" -y "))
                           + QString::number(m_resizeCells));
        m_resizePane = -1;
    });

    connect(m_control, &TmuxControl::ready, this, &TmuxSession::onReady);

    connect(m_control, &TmuxControl::output, this, [this](int pane, const QByteArray &data) {
        if (m_restoring.contains(pane))
            return;   // already in the capture that is on its way
        if (TerminalWidget *p = m_panes.value(pane)) {
            p->feed(data);
            return;
        }
        QByteArray &early = m_earlyOutput[pane];
        if (early.size() < kEarlyOutputLimit)
            early += data;
    });

    connect(m_control, &TmuxControl::windowAdded, this, [this](int window) {
        if (m_windows.contains(window))
            return;
        m_control->command(QStringLiteral("display-message -p -t ") + windowTarget(window)
                               + QStringLiteral(" '#{window_layout} #{window_name}'"),
                           [this, window](bool ok, const QList<QByteArray> &lines) {
            if (!ok || lines.isEmpty())
                return;
            const QByteArray line = lines.first();
            const int space = line.indexOf(' ');
            addWindow(window, line.left(space),
                      space < 0 ? QString() : QString::fromUtf8(line.mid(space + 1)), false);
        });
    });

    connect(m_control, &TmuxControl::layoutChanged, this, [this](int window, const QByteArray &layout) {
        if (!m_windows.contains(window)) {
            addWindow(window, layout, QString(), false);
            return;
        }
        PaneLayout parsed;
        if (TmuxControl::parseLayout(layout, parsed)) {
            m_windows[window].layout = parsed;
            applyLayout(window);
        }
    });

    connect(m_control, &TmuxControl::windowClosed, this, &TmuxSession::removeWindow);

    connect(m_control, &TmuxControl::windowRenamed, this, [this](int window, const QString &name) {
        auto it = m_windows.find(window);
        if (it == m_windows.end())
            return;
        it->name = name;
        if (it->tab)
            emit tabRenamed(it->tab, name);
    });

    connect(m_control, &TmuxControl::activeWindowChanged, this, [this](int window) {
        m_activeWindow = window;
        if (TerminalTab *tab = m_windows.value(window).tab)
            emit tabActivated(tab);
    });

    connect(m_control, &TmuxControl::activePaneChanged, this, [this](int window, int pane) {
        TerminalTab *tab = m_windows.value(window).tab;
        TerminalWidget *p = m_panes.value(pane);
        if (tab && p)
            tab->setActivePane(p, tab->isVisible());
    });

    connect(m_control, &TmuxControl::sessionChanged, this, [this](const QString &name) {
        m_sessionName = name;
    });

    connect(m_control, &TmuxControl::paused, this, &TmuxSession::resyncPane);

    connect(m_control, &TmuxControl::exited, this, [this](const QString &reason) {
        if (m_ended)
            return;
        m_ended = true;
        const QList<int> ids = m_windows.keys();
        for (int id : ids)
            removeWindow(id);
        emit ended(reason);
    });

    // A font change changes the cell size, so every layout and the client
    // size in cells. Padding follows the margin setting.
    connect(&m_settings, &Settings::appearanceChanged, this, [this]() {
        if (m_metrics)
            m_metrics->applyAppearance();
        for (const Window &w : std::as_const(m_windows)) {
            if (w.tab)
                w.tab->setPadding(m_settings.appearance().terminalMargin);
        }
        m_clientCells = QSize();
        relayoutAll();
        m_sizeTimer->start();
    });
}

TmuxSession::~TmuxSession()
{
    delete m_metrics;
}

void TmuxSession::start()
{
    m_control->start(m_command);
}

void TmuxSession::detach()
{
    m_detachRequested = true;
    m_control->detach();
}

void TmuxSession::onReady()
{
    // Flow control (tmux 3.2+): if superterm falls more than a few seconds
    // behind a flood of output, tmux pauses the pane instead of queueing
    // without limit, and resyncPane() catches it up.
    m_control->command(QStringLiteral("display-message -p '#{version}'"),
                       [this](bool ok, const QList<QByteArray> &lines) {
        if (!ok || lines.isEmpty())
            return;
        const auto m = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)"))
                           .match(QString::fromUtf8(lines.first()));
        if (!m.hasMatch())
            return;
        const int major = m.captured(1).toInt();
        const int minor = m.captured(2).toInt();
        if (major > 3 || (major == 3 && minor >= 2))
            m_control->command(QStringLiteral("refresh-client -f pause-after=5"));
    });

    // Size first, so the windows listed next already have the size their
    // tabs will show them at, and their restored history fits.
    sendClientSize();
    restoreWindows();
}

void TmuxSession::restoreWindows()
{
    m_control->command(
        QStringLiteral("list-windows -F '#{window_id} #{window_active} #{window_layout} #{window_name}'"),
        [this](bool ok, const QList<QByteArray> &lines) {
            if (!ok)
                return;
            for (const QByteArray &line : lines) {
                const QList<QByteArray> f = line.split(' ');
                if (f.size() < 3)
                    continue;
                const QString name = QString::fromUtf8(line.mid(
                    f.at(0).size() + f.at(1).size() + f.at(2).size() + 3));
                addWindow(parseId(f.at(0)), f.at(2), name, f.at(1) == "1");
            }

            m_control->command(
                QStringLiteral("list-panes -s -F '#{pane_id} #{alternate_on} #{cursor_x} #{cursor_y}'"),
                [this](bool ok, const QList<QByteArray> &lines) {
                    QSet<int> listed;
                    if (ok) {
                        for (const QByteArray &line : lines) {
                            const QList<QByteArray> f = line.split(' ');
                            if (f.size() < 4)
                                continue;
                            const int id = parseId(f.at(0));
                            listed.insert(id);
                            restorePane(id, f.at(1) == "1", f.at(2).toInt(), f.at(3).toInt());
                        }
                    }
                    // Anything still marked that tmux didn't list will never
                    // get a capture; let its output through.
                    for (auto it = m_restoring.begin(); it != m_restoring.end();) {
                        if (!listed.contains(*it))
                            it = m_restoring.erase(it);
                        else
                            ++it;
                    }
                    m_attaching = false;
                });
        });
}

void TmuxSession::restorePane(int paneId, bool alternate, int cursorX, int cursorY)
{
    m_restoring.insert(paneId);

    // History and screen as they are now, with colours (-e) and wrapped lines
    // rejoined (-J) so they rewrap at this pane's width. A full-screen program
    // gets just its screen, on the alternate screen where it belongs.
    const QString cmd = alternate
        ? QStringLiteral("capture-pane -p -e -t ") + paneTarget(paneId)
        : QStringLiteral("capture-pane -p -e -J -S - -t ") + paneTarget(paneId);

    m_control->command(cmd, [this, paneId, alternate, cursorX, cursorY](bool ok, const QList<QByteArray> &lines) {
        m_restoring.remove(paneId);
        TerminalWidget *pane = m_panes.value(paneId);
        if (!ok || !pane)
            return;
        QByteArray data;
        if (alternate)
            data += "\033[?1049h\033[H\033[2J";
        data += joinCapture(lines);
        data += cursorTo(cursorX, cursorY);
        pane->feed(data);
    });
}

void TmuxSession::resyncPane(int paneId)
{
    // tmux dropped this pane's output while it was paused. Let it continue,
    // then redraw the screen from a capture; anything that arrives in between
    // is drawn over by the capture, which already contains it.
    m_control->command(QStringLiteral("refresh-client -A '") + paneTarget(paneId)
                       + QStringLiteral(":continue'"));
    m_control->command(
        QStringLiteral("display-message -p -t ") + paneTarget(paneId)
            + QStringLiteral(" '#{cursor_x} #{cursor_y}'"),
        [this, paneId](bool ok, const QList<QByteArray> &lines) {
            if (!ok || lines.isEmpty())
                return;
            const QList<QByteArray> f = lines.first().split(' ');
            const int x = f.value(0).toInt();
            const int y = f.value(1).toInt();
            m_control->command(QStringLiteral("capture-pane -p -e -t ") + paneTarget(paneId),
                               [this, paneId, x, y](bool ok, const QList<QByteArray> &lines) {
                TerminalWidget *pane = m_panes.value(paneId);
                if (!ok || !pane)
                    return;
                pane->feed("\033[0m\033[H\033[2J" + joinCapture(lines) + cursorTo(x, y));
            });
        });
}

void TmuxSession::addWindow(int id, const QByteArray &layout, const QString &name, bool active)
{
    if (id < 0)
        return;
    if (active)
        m_activeWindow = id;

    // Known already (a layout notification got here first): what the listing
    // adds is the name and whether it is tmux's current window.
    if (auto it = m_windows.find(id); it != m_windows.end()) {
        if (!name.isEmpty() && name != it->name) {
            it->name = name;
            if (it->tab)
                emit tabRenamed(it->tab, name);
        }
        if (active && it->tab)
            emit tabActivated(it->tab);
        return;
    }

    Window w;
    if (!TmuxControl::parseLayout(layout, w.layout))
        return;
    w.name = name.isEmpty() ? QStringLiteral("tmux") : name;

    auto *tab = new TerminalTab(nullptr);
    tab->setAlwaysFramed(true);
    tab->setPadding(m_settings.appearance().terminalMargin);
    w.tab = tab;
    m_windows.insert(id, w);

    applyLayout(id);

    // Focus moving between panes here moves tmux's active pane too, so the
    // next client to attach sees the same one.
    connect(tab, &TerminalTab::activePaneChanged, this, [this](TerminalWidget *pane) {
        const int pid = paneId(pane);
        if (pid >= 0)
            m_control->command(QStringLiteral("select-pane -t ") + paneTarget(pid));
    });

    connect(tab, &TerminalTab::splitterDragged, this,
            [this](TerminalWidget *before, Qt::Orientation orientation, int pixels) {
        const int pid = paneId(before);
        if (pid < 0)
            return;
        const QSize cell = cellSize();
        const QSize chrome = paneChrome();
        const bool across = orientation == Qt::Horizontal;
        const double px = pixels - (across ? chrome.width() : chrome.height());
        m_resizePane = pid;
        m_resizeOrientation = orientation;
        m_resizeCells = qMax(1, qRound(px / (across ? cell.width() : cell.height())));
        m_resizeTimer->start();
    });

    // Every tab has the same area, so the one on screen resizing resizes the
    // client. Only that one: a tab not yet shown reports a default size.
    connect(tab, &TerminalTab::layoutAreaResized, this, [this, tab]() {
        if (tab->isVisible())
            setClientArea(tab->layoutArea());
    });

    emit tabAdded(tab, w.name);
    // tmux may have switched to this window before it existed here (a new
    // window is announced, then made current, then described).
    if (id == m_activeWindow)
        emit tabActivated(tab);
}

void TmuxSession::removeWindow(int id)
{
    const Window w = m_windows.take(id);
    if (!w.tab)
        return;
    for (TerminalWidget *pane : w.tab->panes())
        m_panes.remove(paneId(pane));
    emit tabRemoved(w.tab);
}

void TmuxSession::applyLayout(int windowId)
{
    auto it = m_windows.find(windowId);
    if (it == m_windows.end() || !it->tab)
        return;

    // Forget panes this window no longer has; the tab deletes them.
    QSet<int> keep;
    collectPaneIds(it->layout, keep);
    for (TerminalWidget *pane : it->tab->panes()) {
        const int pid = paneId(pane);
        if (!keep.contains(pid))
            m_panes.remove(pid);
    }

    it->tab->applyLayout(it->layout, [this](int pid) { return paneFor(pid); },
                         cellSize(), paneChrome());
}

void TmuxSession::relayoutAll()
{
    const QList<int> ids = m_windows.keys();
    for (int id : ids)
        applyLayout(id);
}

TerminalWidget *TmuxSession::paneFor(int paneId)
{
    if (TerminalWidget *pane = m_panes.value(paneId))
        return pane;
    return makePane(paneId);
}

TerminalWidget *TmuxSession::makePane(int paneId)
{
    auto *pane = new TerminalWidget(kPaneIdBase + paneId, m_settings, m_profile, nullptr,
                                    TerminalWidget::Backend::External);
    pane->applyAppearance();
    pane->applyScrolling();
    connect(pane, &TerminalWidget::externalInput, this, [this, paneId](const QByteArray &bytes) {
        sendInput(paneId, bytes);
    });
    m_panes.insert(paneId, pane);

    // While attaching, every pane's history comes from a capture, which
    // includes whatever output arrived first. Panes created later are new and
    // start from that first output.
    const QByteArray early = m_earlyOutput.take(paneId);
    if (m_attaching)
        m_restoring.insert(paneId);
    else if (!early.isEmpty())
        pane->feed(early);
    return pane;
}

void TmuxSession::sendInput(int paneId, const QByteArray &bytes)
{
    // send-keys -H types raw bytes, so everything the emulator produces
    // (keys, pastes, mouse reports) arrives exactly as it would from a pty.
    for (int i = 0; i < bytes.size(); i += kSendChunk) {
        QString cmd = QStringLiteral("send-keys -t ") + paneTarget(paneId) + QStringLiteral(" -H");
        const QByteArray chunk = bytes.mid(i, kSendChunk);
        for (char c : chunk)
            cmd += QStringLiteral(" %1").arg(uchar(c), 2, 16, QLatin1Char('0'));
        m_control->command(cmd);
    }
}

void TmuxSession::setClientArea(const QSize &pixels)
{
    if (pixels.isEmpty() || pixels == m_clientArea)
        return;
    m_clientArea = pixels;
    m_sizeTimer->start();
}

void TmuxSession::sendClientSize()
{
    if (!m_control->isRunning() || m_clientArea.isEmpty())
        return;
    const QSize cell = cellSize();
    const QSize chrome = paneChrome();
    if (cell.isEmpty())
        return;
    const QSize cells(qMax(10, (m_clientArea.width() - chrome.width()) / cell.width()),
                      qMax(3, (m_clientArea.height() - chrome.height()) / cell.height()));
    if (cells == m_clientCells)
        return;
    m_clientCells = cells;
    m_control->command(QStringLiteral("refresh-client -C %1x%2")
                           .arg(cells.width()).arg(cells.height()));
}

void TmuxSession::newWindow()
{
    m_control->command(QStringLiteral("new-window -c '#{pane_current_path}'"));
}

void TmuxSession::splitPane(TerminalWidget *pane, Qt::Orientation orientation)
{
    const int pid = paneId(pane);
    if (pid < 0)
        return;
    // -h puts the new pane beside, -v below; it starts where this one is.
    m_control->command(QStringLiteral("split-window ")
                       + (orientation == Qt::Horizontal ? QStringLiteral("-h") : QStringLiteral("-v"))
                       + QStringLiteral(" -t ") + paneTarget(pid)
                       + QStringLiteral(" -c '#{pane_current_path}'"));
}

void TmuxSession::killPane(TerminalWidget *pane)
{
    const int pid = paneId(pane);
    if (pid >= 0)
        m_control->command(QStringLiteral("kill-pane -t ") + paneTarget(pid));
}

void TmuxSession::killWindow(TerminalTab *tab)
{
    const int wid = windowId(tab);
    if (wid >= 0)
        m_control->command(QStringLiteral("kill-window -t ") + windowTarget(wid));
}

void TmuxSession::renameWindow(TerminalTab *tab, const QString &name)
{
    const int wid = windowId(tab);
    if (wid >= 0)
        m_control->command(QStringLiteral("rename-window -t ") + windowTarget(wid)
                           + QLatin1Char(' ') + TmuxControl::quote(name));
}

void TmuxSession::selectWindow(TerminalTab *tab)
{
    const int wid = windowId(tab);
    if (wid >= 0)
        m_control->command(QStringLiteral("select-window -t ") + windowTarget(wid));
}

bool TmuxSession::ownsTab(const TerminalTab *tab) const
{
    return windowId(tab) >= 0;
}

bool TmuxSession::ownsPane(const TerminalWidget *pane) const
{
    return paneId(pane) >= 0;
}

QList<TerminalTab *> TmuxSession::tabs() const
{
    QList<TerminalTab *> out;
    for (const Window &w : m_windows) {
        if (w.tab)
            out << w.tab;
    }
    return out;
}

QString TmuxSession::windowName(const TerminalTab *tab) const
{
    for (const Window &w : m_windows) {
        if (w.tab == tab)
            return w.name;
    }
    return QString();
}

int TmuxSession::paneId(const TerminalWidget *pane) const
{
    if (!pane)
        return -1;
    for (auto it = m_panes.cbegin(); it != m_panes.cend(); ++it) {
        if (it.value() == pane)
            return it.key();
    }
    return -1;
}

int TmuxSession::windowId(const TerminalTab *tab) const
{
    if (!tab)
        return -1;
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it) {
        if (it->tab == tab)
            return it.key();
    }
    return -1;
}

QSize TmuxSession::cellSize()
{
    // Any pane would do, but there may not be one yet (the first layout is
    // sized before its panes exist), so a hidden one measures.
    if (!m_metrics) {
        m_metrics = new TerminalWidget(kPaneIdBase - 1, m_settings, m_profile, nullptr,
                                       TerminalWidget::Backend::External);
        m_metrics->applyAppearance();
    }
    return m_metrics->cellSize();
}

QSize TmuxSession::paneChrome() const
{
    // tmux panes are always framed and have no margin or scrollbar (see
    // TerminalWidget::applyAppearance()), so this is the same for every pane.
    const int frame = 2 * TerminalWidget::paneBorder();
    return {frame, frame};
}
