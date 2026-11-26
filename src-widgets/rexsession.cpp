/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexsession.h"

#include <functional>

#include <QApplication>

#include "ghosttyreplay.h"
#include "rexclient.h"
#include "settings.h"

#include "TerminalDisplay.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QSet>
#include <QTimer>

#include <cmath>

Q_LOGGING_CATEGORY(lcRex, "puddlejumper.rex")

namespace {

// Pane ids apart from local panes' and tmux's.
constexpr int kPaneIdBase = 200000;
int nextPaneId = 0;

// A write request's data is a JSON string; keystrokes are a few bytes, a
// paste may be large.
constexpr qsizetype kWriteChunk = 32 * 1024;

const QString kShellFlavor = QStringLiteral("com.superlogical.terminal.shell");

QString shortId(const QString &id)
{
    return id.section(QLatin1Char(':'), -1).left(8);
}

QJsonObject shellLayout()
{
    return QJsonObject{{QStringLiteral("block"), QJsonObject{{QStringLiteral("flavor"), kShellFlavor}}}};
}

// A rect as Rex sends it, [x, y, w, h], or as {x, y, w, h}, which older
// atlantis sent.
QRectF rectFrom(const QJsonValue &v)
{
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        return a.size() == 4 ? QRectF(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(), a.at(3).toDouble())
                             : QRectF();
    }
    const QJsonObject o = v.toObject();
    return QRectF(o.value(QLatin1String("x")).toDouble(), o.value(QLatin1String("y")).toDouble(),
                  o.value(QLatin1String("w")).toDouble(), o.value(QLatin1String("h")).toDouble());
}

QJsonArray rectJson(double x, double y, double w, double h)
{
    return QJsonArray{x, y, w, h};
}

QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

// Whether any block_id appears anywhere in a view.
bool hasBlocks(const QJsonValue &v)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (!str(o, "block_id").isEmpty())
            return true;
        for (auto it = o.begin(); it != o.end(); ++it)
            if (hasBlocks(it.value()))
                return true;
    } else if (v.isArray()) {
        for (const QJsonValue &e : v.toArray())
            if (hasBlocks(e))
                return true;
    }
    return false;
}

// The blocks of a layout tree, in order.
void layoutBlocks(const QJsonObject &node, QStringList &out)
{
    if (const QString id = str(node, "block_id"); !id.isEmpty()) {
        out << id;
        return;
    }
    const QJsonObject split = node.value(QLatin1String("split")).toObject();
    if (split.isEmpty())
        return;
    layoutBlocks(split.value(QLatin1String("before")).toObject(), out);
    layoutBlocks(split.value(QLatin1String("after")).toObject(), out);
}

QString firstBlock(const QJsonObject &node)
{
    QStringList ids;
    layoutBlocks(node, ids);
    return ids.value(0);
}

// A window's tiled layer: the one layout panes are shown for.
QJsonObject tiledLayout(const QJsonObject &window)
{
    for (const QJsonValue &l : window.value(QLatin1String("layers")).toArray()) {
        const QJsonObject layer = l.toObject();
        if (str(layer, "kind") == QLatin1String("tiled"))
            return layer.value(QLatin1String("layout")).toObject();
    }
    return {};
}

// A split's two parts, in cells: one cell between them for the handle.
QPair<int, int> divide(int total, double ratio)
{
    const int avail = total - 1;
    if (avail < 2)
        return {qMax(1, avail), 1};
    const int before = qBound(1, int(std::lround(avail * ratio)), avail - 1);
    return {before, avail - before};
}

// Rex's proportional layout as cells, for TerminalTab::applyLayout().
PaneLayout toPaneLayout(const QJsonObject &node, int columns, int lines,
                        const std::function<int(const QString &)> &paneId)
{
    PaneLayout out;
    out.columns = columns;
    out.lines = lines;
    if (const QString id = str(node, "block_id"); !id.isEmpty()) {
        out.paneId = paneId(id);
        return out;
    }
    const QJsonObject split = node.value(QLatin1String("split")).toObject();
    const bool across = str(split, "direction") != QLatin1String("vertical");
    const double ratio = split.value(QLatin1String("ratio")).toDouble(0.5);
    out.orientation = across ? Qt::Horizontal : Qt::Vertical;
    const auto [a, b] = divide(across ? columns : lines, ratio);
    out.children << toPaneLayout(split.value(QLatin1String("before")).toObject(),
                                 across ? a : columns, across ? lines : a, paneId);
    out.children << toPaneLayout(split.value(QLatin1String("after")).toObject(),
                                 across ? b : columns, across ? lines : b, paneId);
    return out;
}

void collectLeaves(const PaneLayout &l, QHash<int, QSize> &out)
{
    if (l.isLeaf()) {
        out.insert(l.paneId, QSize(l.columns, l.lines));
        return;
    }
    for (const PaneLayout &c : l.children)
        collectLeaves(c, out);
}

// The splits of a layout in one direction: the first block on each side,
// the before part's extent in cells, and the split's whole extent.
struct SplitInfo
{
    QString beforeBlock, afterBlock;
    int beforeCells = 0, totalCells = 0;
};

void collectSplits(const QJsonObject &node, int columns, int lines, bool across, QList<SplitInfo> &out)
{
    if (!str(node, "block_id").isEmpty())
        return;
    const QJsonObject split = node.value(QLatin1String("split")).toObject();
    if (split.isEmpty())
        return;
    const bool here = str(split, "direction") != QLatin1String("vertical");
    const auto [a, b] = divide(here ? columns : lines, split.value(QLatin1String("ratio")).toDouble(0.5));
    const QJsonObject before = split.value(QLatin1String("before")).toObject();
    const QJsonObject after = split.value(QLatin1String("after")).toObject();
    if (here == across)
        out.append({firstBlock(before), firstBlock(after), a, here ? columns : lines});
    collectSplits(before, here ? a : columns, here ? lines : a, across, out);
    collectSplits(after, here ? b : columns, here ? lines : b, across, out);
}

QString colourName(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

} // namespace

RexSession::RexSession(const QUrl &server, const QString &sessionId, const Settings &settings,
                       const TerminalProfile &profile, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_sessionId(sessionId)
    , m_settings(settings)
    , m_profile(profile)
{
    m_control = new RexControl(this);
    connect(m_control, &RexControl::event, this, &RexSession::onEvent);
    // A margin or pane frame change (pane cards) changes what fits: queued,
    // after the panes have taken the new appearance.
    connect(&m_settings, &Settings::appearanceChanged, this, [this]() {
        // The hidden pane that measures cells isn't among the window's
        // panes: a font change reaches it here, or every layout (and the
        // sizes the server is told) stays at the old cell size.
        if (m_metrics)
            m_metrics->applyAppearance();
        for (Window &w : m_windows) {
            if (!w.tab)
                continue;
            w.tab->setPadding(m_settings.appearance().terminalMargin);
            layoutWindow(w);
        }
    }, Qt::QueuedConnection);
    connect(m_control, &RexControl::closed, this, [this](const QString &reason) {
        finish(reason.isEmpty() ? tr("the connection to the server closed") : reason);
    });
    m_viewTimer = new QTimer(this);
    m_viewTimer->setSingleShot(true);
    m_viewTimer->setInterval(20);
    connect(m_viewTimer, &QTimer::timeout, this, &RexSession::fetchView);
}

RexSession::~RexSession()
{
    // Tabs belong to the window by now (or are gone); panes not in a tab are
    // this session's.
    for (auto &[id, b] : m_blocks)
        if (b->pane && !b->pane->parent())
            delete b->pane;
    delete m_metrics;
}

// ---- Lookups. ---------------------------------------------------------------

QList<TerminalTab *> RexSession::tabs() const
{
    QList<TerminalTab *> out;
    for (const Window &w : m_windows)
        if (w.tab)
            out << w.tab;
    return out;
}

bool RexSession::ownsTab(const Tab *tab) const
{
    for (const Window &w : m_windows)
        if (tab && w.tab && static_cast<const Tab *>(w.tab.data()) == tab)
            return true;
    return false;
}

bool RexSession::ownsPane(const TerminalWidget *pane) const
{
    return pane && paneForBlock(m_paneBlocks.value(pane->id())) == pane;
}

RexSession::Window *RexSession::windowOf(const TerminalTab *tab)
{
    for (Window &w : m_windows)
        if (tab && w.tab == tab)
            return &w;
    return nullptr;
}

const RexSession::Window *RexSession::windowOf(const TerminalTab *tab) const
{
    return const_cast<RexSession *>(this)->windowOf(tab);
}

QString RexSession::windowName(const TerminalTab *tab) const
{
    const Window *w = windowOf(tab);
    return w ? w->label : QString();
}

QString RexSession::blockId(const TerminalWidget *pane) const
{
    return pane ? m_paneBlocks.value(pane->id()) : QString();
}

TerminalWidget *RexSession::paneForBlock(const QString &blockId) const
{
    const auto it = m_blocks.find(blockId);
    return it == m_blocks.end() ? nullptr : it->second->pane.data();
}

bool RexSession::hasExited(const TerminalWidget *pane) const
{
    const auto it = m_blocks.find(blockId(pane));
    return it != m_blocks.end() && it->second->exited;
}

QJsonObject RexSession::target(const Block *b) const
{
    return QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("block_id"), b->id}};
}

QSize RexSession::cellSize()
{
    // Any pane would do, but there may not be one yet, so a hidden one
    // measures (as for tmux).
    if (!m_metrics) {
        m_metrics = new TerminalWidget(kPaneIdBase - 1, m_settings, m_profile, nullptr,
                                       TerminalWidget::Backend::External);
        m_metrics->applyAppearance();
    }
    return m_metrics->cellSize();
}

QSize RexSession::paneChrome() const
{
    // Rex tabs are always framed, like tmux's, so a pane's chrome is the
    // same split or not (a tiled pane's: a card's with pane cards on).
    return TerminalWidget::frameChrome(true);
}

// ---- Attaching and following the view. -------------------------------------

void RexSession::start()
{
    m_control->open(m_server);
    if (!m_sessionId.isEmpty()) {
        attach();
        return;
    }
    m_control->request(
        QStringLiteral("session.create"),
        QJsonObject{{QStringLiteral("initial_windows"),
                     QJsonArray{QJsonObject{{QStringLiteral("layout"), shellLayout()}}}}},
        [this](const QJsonObject &payload, const QJsonObject &error) {
            m_sessionId = str(payload, "session_id");
            if (!error.isEmpty() || m_sessionId.isEmpty()) {
                finish(tr("could not create a session: %1").arg(str(error, "message")));
                return;
            }
            attach();
        });
}

void RexSession::attach()
{
    m_control->request(QStringLiteral("session.attach"), QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                       [this](const QJsonObject &payload, const QJsonObject &error) {
                           if (!error.isEmpty()) {
                               finish(tr("could not attach: %1").arg(str(error, "message")));
                               return;
                           }
                           m_attached = true;
                           qCInfo(lcRex) << "attached" << m_sessionId << "on" << m_server;
                           applyView(payload.value(QLatin1String("view")).toObject());
                           requestBlocks();
                           sendScripts();  // then the keymap
                       });
}

void RexSession::fetchView()
{
    if (m_finished || !m_attached)
        return;
    if (m_viewPending) {
        m_viewAgain = true;
        return;
    }
    m_viewPending = true;
    m_control->request(QStringLiteral("session.view"), QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                       [this](const QJsonObject &view, const QJsonObject &error) {
                           m_viewPending = false;
                           if (m_finished)
                               return;
                           if (!error.isEmpty()) {
                               if (str(error, "code") == QLatin1String("not_found"))
                                   finish(tr("the session was ended"));
                               return;
                           }
                           applyView(view);
                           if (std::exchange(m_viewAgain, false))
                               fetchView();
                       });
}

void RexSession::applyView(const QJsonObject &view)
{
    if (m_finished)
        return;
    const qint64 revision = view.value(QLatin1String("revision")).toInteger(-1);
    if (revision >= 0 && revision < m_revision)
        return;  // older than one applied already
    m_revision = revision;

    const QString label = str(view, "label");
    const QString oldLabel = m_label;
    m_label = label.isEmpty() ? shortId(m_sessionId) : label;
    m_activeWindow = str(view, "active_window_id");

    // Block labels, wherever the view has them (tiled or floating): a pane's
    // title until its program sets one.
    std::function<void(const QJsonValue &)> labels = [&](const QJsonValue &v) {
        if (v.isArray()) {
            for (const QJsonValue &e : v.toArray())
                labels(e);
            return;
        }
        if (!v.isObject())
            return;
        const QJsonObject o = v.toObject();
        const QString id = str(o, "block_id");
        if (!id.isEmpty() && o.contains(QLatin1String("label")) && m_blocks.count(id)) {
            Block *b = block(id);
            b->label = str(o, "label");
            if (b->pane)
                b->pane->setFallbackTitle(b->label);
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it->isObject() || it->isArray())
                labels(*it);
    };
    labels(view.value(QLatin1String("windows")));

    // Windows in the server's order, each with the blocks of its tiled layout.
    QList<Window> next;
    QSet<QString> live;
    for (const QJsonValue &v : view.value(QLatin1String("windows")).toArray()) {
        const QJsonObject wo = v.toObject();
        Window w;
        w.id = str(wo, "window_id");
        w.label = str(wo, "label");
        w.layout = tiledLayout(wo);
        w.focusedBlock = str(wo, "focused_block_id");
        w.zoomedBlock = str(wo, "zoomed_block_id");
        w.tiledZoom = str(wo, "tiled_zoomed_block_id");
        for (const Window &old : std::as_const(m_windows))
            if (old.id == w.id)
                w.tab = old.tab;
        QStringList ids;
        layoutBlocks(w.layout, ids);
        for (const QJsonValue &lv : wo.value(QLatin1String("layers")).toArray()) {
            const QJsonObject lo = lv.toObject();
            if (str(lo, "kind") != QLatin1String("floating"))
                continue;
            Window::Layer layer;
            layer.id = str(lo, "layer_id");
            layer.bounds = rectFrom(lo.value(QLatin1String("bounds")));
            layer.layout = lo.value(QLatin1String("layout")).toObject();
            for (const QJsonValue &bv : lo.value(QLatin1String("blocks")).toArray()) {
                const QJsonObject bo = bv.toObject();
                if (Block *known = m_blocks.count(str(bo, "block_id")) ? block(str(bo, "block_id")) : nullptr)
                    known->label = str(bo, "label");
            }
            const qsizetype had = ids.size();
            layoutBlocks(layer.layout, ids);
            if (ids.size() > had && layer.bounds.isValid())
                w.floating << layer;
        }
        if (ids.isEmpty())
            continue;  // nothing to show
        for (const QString &id : std::as_const(ids))
            live.insert(id);
        next << w;
    }

    // Windows gone: their tabs go, and their panes with them.
    for (const Window &old : std::as_const(m_windows)) {
        bool kept = false;
        for (const Window &w : std::as_const(next))
            kept |= w.id == old.id;
        if (!kept && old.tab)
            emit tabRemoved(old.tab);
    }
    // Blocks gone: their streams close.
    QStringList gone;
    for (const auto &[id, b] : m_blocks)
        if (!live.contains(id))
            gone << id;
    for (const QString &id : std::as_const(gone))
        dropBlock(id);

    const QList<Window> previous = std::exchange(m_windows, next);
    for (Window &w : m_windows) {
        QString oldLabel;
        bool existed = false;
        for (const Window &old : previous)
            if (old.id == w.id) {
                existed = true;
                oldLabel = old.label;
            }
        layoutWindow(w);
        if (existed && w.tab && oldLabel != w.label)
            emit tabRenamed(w.tab, w.label);
    }
    // Who owns each block (left out: no one).
    for (const QJsonValue &v : view.value(QLatin1String("windows")).toArray())
        for (const QJsonValue &lv : v.toObject().value(QLatin1String("layers")).toArray())
            for (const QJsonValue &bv : lv.toObject().value(QLatin1String("blocks")).toArray()) {
                const QJsonObject bo = bv.toObject();
                auto it = m_blocks.find(str(bo, "block_id"));
                if (it != m_blocks.end())
                    setOwner(it->second.get(), str(bo, "owner"));
            }
    if (m_windows.isEmpty())
        endIfEmpty();
    else if (!oldLabel.isEmpty() && oldLabel != m_label)
        emit labelChanged(m_label);
}

QSize RexSession::windowArea(const Window &w)
{
    QSize area = w.tab && w.tab->isVisible() ? w.tab->layoutArea() : m_area;
    if (area.isEmpty()) {
        const QSize cell = cellSize();
        const QSize chrome = paneChrome();
        area = QSize(80 * cell.width() + chrome.width(), 24 * cell.height() + chrome.height());
    }
    return area;
}

QRect RexSession::floatingFrame(const QRectF &bounds, const QSize &area, QSize *cells)
{
    // The frame is as much of the bounds as whole cells fill.
    const QSize cell = cellSize();
    const QSize chrome = TerminalWidget::frameChrome(false) + TerminalTab::floatingChrome();
    const QRect box(qRound(bounds.x() * area.width()), qRound(bounds.y() * area.height()),
                    qRound(bounds.width() * area.width()), qRound(bounds.height() * area.height()));
    const int columns = qMax(4, (box.width() - chrome.width()) / qMax(1, cell.width()));
    const int lines = qMax(1, (box.height() - chrome.height()) / qMax(1, cell.height()));
    if (cells)
        *cells = QSize(columns, lines);
    return QRect(box.topLeft(), QSize(columns * cell.width() + chrome.width(), lines * cell.height() + chrome.height()));
}

void RexSession::layoutWindow(Window &w)
{
    const QSize cell = cellSize();
    const QSize chrome = paneChrome();
    const QSize area = windowArea(w);
    int columns = qMax(10, (area.width() - chrome.width()) / qMax(1, cell.width()));
    int lines = qMax(3, (area.height() - chrome.height()) / qMax(1, cell.height()));

    const auto paneId = [this](const QString &id) { return block(id)->paneId; };
    // A window whose blocks all float has no tiled pane (-1: none).
    PaneLayout layout;
    if (!firstBlock(w.layout).isEmpty()) {
        layout = toPaneLayout(w.layout, columns, lines, paneId);
        // What the panes' frames and gaps take past the separator cells
        // (pane cards): cells given up to make room, the same shape again.
        const QSize over = TerminalTab::layoutOverflow(layout, cell, chrome);
        if (over.width() > 0 || over.height() > 0) {
            columns = qMax(10, columns - (over.width() + cell.width() - 1) / qMax(1, cell.width()));
            lines = qMax(3, lines - (over.height() + cell.height() - 1) / qMax(1, cell.height()));
            layout = toPaneLayout(w.layout, columns, lines, paneId);
        }
    }
    QList<FloatingLayout> floating;
    for (const Window::Layer &l : std::as_const(w.floating)) {
        QSize cells;
        const QRect frame = floatingFrame(l.bounds, area, &cells);
        const QString first = firstBlock(l.layout);
        floating.append({l.id, frame.topLeft(), toPaneLayout(l.layout, cells.width(), cells.height(), paneId),
                         first.isEmpty() ? QString() : block(first)->label});
    }
    // Zoomed: the block fills the window; the others are kept, hidden, at
    // the sizes they had. A zoom over everything (a floating block's, or an
    // older server's only one) hides every layer; the tiled zoom (revision
    // 21) hides only the rest of the tiled layer, under the floating ones.
    QList<int> hidden;
    QStringList tiledIds;
    layoutBlocks(w.layout, tiledIds);
    const QString overAll = w.zoomedBlock != w.tiledZoom ? w.zoomedBlock : QString();
    if (!overAll.isEmpty() && m_blocks.count(overAll)) {
        QStringList all = tiledIds;
        for (const Window::Layer &l : std::as_const(w.floating))
            layoutBlocks(l.layout, all);
        if (all.contains(overAll)) {
            for (const QString &id : std::as_const(all))
                if (id != overAll)
                    hidden << block(id)->paneId;
            layout = PaneLayout();
            layout.paneId = block(overAll)->paneId;
            layout.columns = columns;
            layout.lines = lines;
            floating.clear();
        }
    } else if (!w.tiledZoom.isEmpty() && m_blocks.count(w.tiledZoom) && tiledIds.contains(w.tiledZoom)) {
        for (const QString &id : std::as_const(tiledIds))
            if (id != w.tiledZoom)
                hidden << block(id)->paneId;
        layout = PaneLayout();
        layout.paneId = block(w.tiledZoom)->paneId;
        layout.columns = columns;
        layout.lines = lines;
    }

    const bool fresh = !w.tab;
    if (fresh) {
        w.tab = new TerminalTab(nullptr);
        w.tab->setAlwaysFramed(true);
        w.tab->setPadding(m_settings.appearance().terminalMargin);
        w.tab->setPaneControls(true);
        TerminalTab *tab = w.tab;
        connect(tab, &TerminalTab::paneZoomRequested, this, [this](TerminalWidget *pane) { toggleZoom(pane); });
        connect(tab, &TerminalTab::paneFloatRequested, this, [this](TerminalWidget *pane) {
            if (isFloating(pane))
                dockPane(pane);
            else
                floatPane(pane);
        });
        // The pane with the focus here is the focused block on the server.
        connect(tab, &TerminalTab::activePaneChanged, this, [this, tab](TerminalWidget *pane) {
            const QString id = blockId(pane);
            const Window *win = windowOf(tab);
            if (m_followingFocus || (win && win->focusedBlock == id))
                return;  // the server's already
            if (!id.isEmpty() && !m_finished)
                m_control->request(QStringLiteral("session.focus_block"),
                                   QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                               {QStringLiteral("block_id"), id}});
        });
        connect(tab, &TerminalTab::splitterDragged, this,
                [this, tab](TerminalWidget *before, Qt::Orientation orientation, int pixels) {
            if (Window *win = windowOf(tab))
                splitterDragged(*win, before, orientation, pixels);
        });
        connect(tab, &TerminalTab::floatingGeometryChanged, this,
                [this, tab](const QString &layerId, const QRect &frame) {
            if (Window *win = windowOf(tab))
                floatingMoved(*win, layerId, frame);
        });
        connect(tab, &TerminalTab::floatingCloseRequested, this, [this](const QString &layerId) {
            if (!m_finished)
                m_control->request(QStringLiteral("session.close_layer"),
                                   QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                               {QStringLiteral("layer_id"), layerId}});
        });
        connect(tab, &TerminalTab::floatingRaiseRequested, this, [this](const QString &layerId) {
            if (!m_finished)
                m_control->request(QStringLiteral("session.raise_layer"),
                                   QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                               {QStringLiteral("layer_id"), layerId}});
        });
        connect(tab, &TerminalTab::layoutAreaResized, this, [this, tab]() {
            if (!tab->isVisible())
                return;
            m_area = tab->layoutArea();
            if (Window *win = windowOf(tab))
                layoutWindow(*win);
        });
    }

    w.tab->applyLayout(layout, [this](int paneId) -> TerminalWidget * {
        if (paneId < 0)
            return nullptr;
        Block *b = block(m_paneBlocks.value(paneId));
        TerminalWidget *pane = b->pane ? b->pane.data() : makePane(b);
        if (pane)
            pane->setFallbackTitle(b->label);
        return pane;
    }, cell, chrome, floating, hidden);
    {
        QList<TerminalWidget *> zoomed;
        for (const QString &id : {w.zoomedBlock, w.tiledZoom})
            if (!id.isEmpty() && m_blocks.count(id) && block(id)->pane)
                zoomed << block(id)->pane.data();
        w.tab->setZoomedPanes(zoomed);
    }

    // The server's focused block is the active pane here too (it decides
    // which pane a zoom shows, and focusing another ends a zoom). Keyboard
    // focus moves with it only if it was in this tab already.
    if (Block *fb = w.focusedBlock.isEmpty() || !m_blocks.count(w.focusedBlock) ? nullptr : block(w.focusedBlock);
        fb && fb->pane && w.tab->panes().contains(fb->pane.data()) && w.tab->activePane() != fb->pane) {
        const bool hadFocus = w.tab->isAncestorOf(QApplication::focusWidget());
        m_followingFocus = true;
        w.tab->setActivePane(fb->pane, hadFocus);
        m_followingFocus = false;
    }

    // Each block the size its pane has; a block's stream opens once it has
    // been sized, so its snapshot comes at that size.
    QHash<int, QSize> sizes;
    collectLeaves(layout, sizes);
    for (const FloatingLayout &f : std::as_const(floating))
        collectLeaves(f.layout, sizes);
    sizes.remove(-1);
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
        Block *b = block(m_paneBlocks.value(it.key()));
        sendSize(b, it.value());
        // Another client's block keeps its size: the pane shows that, not
        // the layout's.
        if (following(b) && b->pane && !b->screen.isEmpty())
            b->pane->setScreenSize(b->screen.width(), b->screen.height());
    }

    if (fresh)
        emit tabAdded(w.tab, w.label);
}

RexSession::Block *RexSession::block(const QString &id)
{
    auto it = m_blocks.find(id);
    if (it != m_blocks.end())
        return it->second.get();
    auto b = std::make_unique<Block>();
    b->id = id;
    b->paneId = kPaneIdBase + nextPaneId++;
    m_paneBlocks.insert(b->paneId, id);
    Block *raw = b.get();
    m_blocks.emplace(id, std::move(b));
    return raw;
}

TerminalWidget *RexSession::makePane(Block *b)
{
    auto *pane = new TerminalWidget(b->paneId, m_settings, m_profile, nullptr, TerminalWidget::Backend::External);
    pane->applyAppearance();
    pane->applyScrolling();
    const QString id = b->id;
    connect(pane, &TerminalWidget::externalInput, this, [this, id](const QByteArray &bytes) {
        auto it = m_blocks.find(id);
        if (it != m_blocks.end())
            sendInput(it->second.get(), bytes);
    });
    connect(pane, &TerminalWidget::pointerPressed, this, [this, id]() {
        auto it = m_blocks.find(id);
        if (it != m_blocks.end())
            takeBlock(it->second.get());
    });
    b->pane = pane;
    sendTheme(b);
    showFollowing(b);
    return pane;
}

void RexSession::openStream(Block *b)
{
    if (b->stream || m_finished)
        return;
    b->stream = new RexTerminalConnection(m_control, m_sessionId, b->id, this);
    const QString id = b->id;
    auto find = [this, id]() -> Block * {
        auto it = m_blocks.find(id);
        return it == m_blocks.end() ? nullptr : it->second.get();
    };
    connect(b->stream, &RexTerminalConnection::snapshot, this, [this, find](const GhosttySnapshot::Snapshot &s) {
        if (Block *blk = find())
            onSnapshot(blk, s);
    });
    connect(b->stream, &RexTerminalConnection::output, this, [find](const QByteArray &bytes) {
        if (Block *blk = find(); blk && blk->pane)
            blk->pane->feed(bytes);
    });
    connect(b->stream, &RexTerminalConnection::marker, this, [this, find](const QJsonObject &m) {
        if (Block *blk = find())
            onMarker(blk, m);
    });
    connect(b->stream, &RexTerminalConnection::interrupted, this, [id]() {
        qCInfo(lcRex) << id << "stream interrupted; resuming";
    });
    connect(b->stream, &RexTerminalConnection::failed, this, [this, id](const QString &reason) {
        // A block that has gone takes its stream with it, and the view
        // change says so.
        qCInfo(lcRex) << id << "stream ended:" << reason;
        m_viewTimer->start();
    });
    b->stream->open();
}

void RexSession::dropBlock(const QString &id)
{
    auto it = m_blocks.find(id);
    if (it == m_blocks.end())
        return;
    Block *b = it->second.get();
    if (b->stream) {
        b->stream->close();
        b->stream->deleteLater();
    }
    // A pane in a tab goes when the tab's layout drops it, or with the tab;
    // one never put in a tab is ours.
    if (b->pane && !b->pane->parent())
        b->pane->deleteLater();
    m_paneBlocks.remove(b->paneId);
    m_blocks.erase(it);
}

void RexSession::onSnapshot(Block *b, const GhosttySnapshot::Snapshot &snapshot)
{
    b->screen = QSize(snapshot.terminal.columns, snapshot.terminal.rows);
    qCDebug(lcRex) << "snapshot" << b->id.right(8) << b->screen;
    if (!b->pane)
        return;
    // The replay resets the screen, history included, so a later snapshot
    // (a resync, or after the stream couldn't be resumed) rebuilds the pane.
    b->pane->setScreenSize(b->screen.width(), b->screen.height());
    b->pane->feed(GhosttySnapshot::replay(snapshot));
    showFollowing(b);
    checkSize(b);
}

void RexSession::onMarker(Block *b, const QJsonObject &marker)
{
    const QString type = str(marker, "type");
    if (type == QLatin1String("resize")) {
        if (marker.contains(QLatin1String("owner")))  // atlantis's; owner events say the rest
            setOwner(b, str(marker, "owner"));
        const QSize size(marker.value(QLatin1String("cols")).toInt(), marker.value(QLatin1String("rows")).toInt());
        qCDebug(lcRex) << "marker" << b->id.right(8) << size << "screen" << b->screen;
        if (size.isEmpty() || size == b->screen) {
            checkSize(b);
            return;
        }
        const bool widthChanged = size.width() != b->screen.width();
        b->screen = size;
        if (b->pane)
            b->pane->setScreenSize(size.width(), size.height());
        // Ghostty rewraps lines to a new width and qtermwidget doesn't: only
        // a fresh snapshot shows what the server has.
        if (widthChanged && b->stream)
            b->stream->resync();
        showFollowing(b);
        checkSize(b);
    } else if (type == QLatin1String("child_exited")) {
        b->exited = true;
        b->exitCode = marker.value(QLatin1String("exit_code")).toInt();
    }
    // theme: what another client asked for. Panes keep the profile's colours.
}

void RexSession::onEvent(const QJsonObject &message)
{
    // {"type":"event", "payload":{"type":..., "payload":{...}}}; a session's
    // events are session_event, with {"session_id", "event":{type, payload}}.
    const QJsonObject outer = message.value(QLatin1String("payload")).toObject();
    const QString kind = str(outer, "type");
    const QJsonObject body = outer.value(QLatin1String("payload")).toObject();
    // Calls from the server, and its configuration changing.
    if (kind == QLatin1String("client_perform_requested")) {
        const QString callId = str(body, "call_id");
        if (callId.isEmpty())
            return;
        TerminalWidget *pane = str(body, "session_id") == m_sessionId ? paneForBlock(str(body, "block_id")) : nullptr;
        emit performRequested(callId, str(body, "action"), body.value(QLatin1String("args")).toObject(), pane);
        return;
    }
    if (kind == QLatin1String("client_prompt_requested")) {
        const QString callId = str(body, "call_id");
        const QJsonObject prompt = body.value(QLatin1String("prompt")).toObject();
        if (callId.isEmpty())
            return;
        if (str(prompt, "title").isEmpty() || (str(prompt, "kind") != QLatin1String("input")
                                               && str(prompt, "kind") != QLatin1String("pick"))) {
            replyCall(callId, {}, QJsonObject{{QStringLiteral("code"), QStringLiteral("not_supported")},
                                              {QStringLiteral("message"), QStringLiteral("PuddleJumper can't show this prompt")}});
            return;
        }
        emit promptRequested(callId, prompt);
        return;
    }
    if (kind == QLatin1String("client_prompt_ended")) {
        emit promptEnded(str(body, "call_id"));
        return;
    }
    if (kind == QLatin1String("client_call_requested")) {
        if (!str(body, "call_id").isEmpty())
            replyCall(str(body, "call_id"), {},
                      QJsonObject{{QStringLiteral("code"), QStringLiteral("not_supported")},
                                  {QStringLiteral("message"), QStringLiteral("PuddleJumper can't reach other servers for actions")}});
        return;
    }
    if (kind == QLatin1String("keymap_changed")) {
        if (!m_keymap.isLoaded() || str(body, "hash") != m_keymap.hash())
            fetchKeymap();
        return;
    }
    if (kind == QLatin1String("session_destroyed")) {
        if (str(body, "session_id") == m_sessionId)
            finish(tr("the session was ended"));
        return;
    }
    if (kind != QLatin1String("session_event") || str(body, "session_id") != m_sessionId)
        return;
    const QJsonObject event = body.value(QLatin1String("event")).toObject();
    const QString type = str(event, "type");
    const QJsonObject payload = event.value(QLatin1String("payload")).toObject();
    if (type == QLatin1String("session_view_changed") || type == QLatin1String("block_closed")) {
        m_viewTimer->start();
    } else if (type == QLatin1String("block_owner_changed")) {
        auto it = m_blocks.find(str(payload, "block_id"));
        if (it != m_blocks.end())
            setOwner(it->second.get(), str(payload, "owner"));
    } else if (type == QLatin1String("block_event")
               && str(payload, "name") == QLatin1String("child_exited")) {
        auto it = m_blocks.find(str(payload, "block_id"));
        if (it != m_blocks.end()) {
            it->second->exited = true;
            it->second->exitCode = payload.value(QLatin1String("payload")).toObject()
                                       .value(QLatin1String("exit_code")).toInt();
        }
    }
}

// ---- Keymaps and actions. ---------------------------------------------------

void RexSession::fetchKeymap()
{
    if (!m_keymapRequest || m_finished)
        return;
    if (m_keymapPending) {
        m_keymapAgain = true;  // it may be from before a change
        return;
    }
    m_keymapPending = true;
    m_control->request(QStringLiteral("client.keymap"), m_keymapRequest(),
                       [this](const QJsonObject &payload, const QJsonObject &error) {
                           m_keymapPending = false;
                           if (m_finished)
                               return;
                           if (m_keymapAgain) {
                               m_keymapAgain = false;
                               fetchKeymap();
                               return;
                           }
                           if (!error.isEmpty()) {
                               // An older server: no keymap, the app's own shortcuts stand.
                               qCInfo(lcRex) << "no keymap:" << str(error, "message");
                               return;
                           }
                           m_keymap.parse(payload);
                           for (const QString &d : m_keymap.diagnostics())
                               qCWarning(lcRex) << "keymap:" << d;
                           emit keymapChanged();
                       });
}

void RexSession::runAction(const QString &name, const QJsonObject &args, const TerminalWidget *pane,
                           const QString &origin)
{
    QJsonObject p{{QStringLiteral("action"), name}, {QStringLiteral("origin"), origin},
                  {QStringLiteral("session_id"), m_sessionId}};
    if (!args.isEmpty())
        p.insert(QStringLiteral("args"), args);
    const QString block = pane ? blockId(pane) : QString();
    if (!block.isEmpty())
        p.insert(QStringLiteral("block_id"), block);
    m_control->request(QStringLiteral("client.action"), p,
                       [this, name](const QJsonObject &payload, const QJsonObject &error) {
                           if (!error.isEmpty())
                               emit actionFailed(name, str(error, "message").isEmpty() ? str(error, "code")
                                                                                       : str(error, "message"));
                           else
                               emit actionFinished(name, payload.value(QLatin1String("result")));
                       });
}

void RexSession::setScripts(const QJsonArray &scripts,
                            std::function<void(const QJsonObject &, const QJsonObject &)> done)
{
    if (m_finished)
        return;
    m_control->request(QStringLiteral("client.set_script"), QJsonObject{{QStringLiteral("scripts"), scripts}},
                       [this, done](const QJsonObject &payload, const QJsonObject &error) {
                           fetchKeymap();  // with the handlers' bindings
                           if (done)
                               done(payload, error);
                       });
}

void RexSession::sendScripts(std::function<void(bool ok, const QString &message)> done)
{
    const auto report = [done](bool ok, const QString &message) {
        if (done)
            done(ok, message);
    };
    if (m_finished || !m_attached) {
        report(false, tr("not connected yet"));
        return;
    }
    const QString path = m_settings.rexScriptsFile();
    if (path.isEmpty() && !m_scriptsSent) {
        fetchKeymap();
        report(true, {});
        return;
    }
    QJsonArray scripts;
    const QString name = QFileInfo(path).fileName();
    if (!path.isEmpty()) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            emit actionFailed(name, f.errorString());
            fetchKeymap();
            report(false, f.errorString());
            return;
        }
        scripts << QJsonObject{{QStringLiteral("name"), name},
                               {QStringLiteral("source"), QString::fromUtf8(f.readAll())}};
    }
    const QString server = m_server.host();
    setScripts(scripts, [this, name, server, report, sent = !scripts.isEmpty()](const QJsonObject &,
                                                                                const QJsonObject &error) {
        if (error.isEmpty()) {
            m_scriptsSent = sent;
            qCInfo(lcRex) << (sent ? "scripts sent:" : "scripts removed") << name;
            report(true, {});
        } else if (str(error, "code") == QLatin1String("method_not_found")) {
            // An older server: quiet on attaching, said when asked.
            qCInfo(lcRex) << "the server takes no client scripts";
            report(false, tr("%1 doesn't take client scripts (no client.set_script)").arg(server));
        } else {
            // The scripts before (if any) stay in use.
            emit actionFailed(name, str(error, "message"));
            report(false, str(error, "message"));
        }
    });
}

void RexSession::forwardKey(const TerminalWidget *pane, const QJsonObject &key, const QStringList &prefix)
{
    const QString block = pane ? blockId(pane) : QString();
    if (block.isEmpty() || m_finished)
        return;
    QJsonObject p{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("block_id"), block},
                  {QStringLiteral("key"), key}};
    if (!prefix.isEmpty())
        p.insert(QStringLiteral("prefix"), QJsonArray::fromStringList(prefix));
    m_control->request(QStringLiteral("client.key"), p, [this](const QJsonObject &, const QJsonObject &error) {
        if (!error.isEmpty())
            emit actionFailed(QStringLiteral("client.key.forward"),
                              str(error, "message").isEmpty() ? str(error, "code") : str(error, "message"));
    });
}

void RexSession::replyCall(const QString &callId, const QJsonValue &result, const QJsonObject &error)
{
    QJsonObject p{{QStringLiteral("call_id"), callId}};
    if (!error.isEmpty())
        p.insert(QStringLiteral("error"), error);
    else
        p.insert(QStringLiteral("result"), result);
    // A reply nobody waits for any more is not_found: nothing to do.
    m_control->request(QStringLiteral("client.reply"), p);
}

QString RexSession::clientId() const
{
    return m_control->clientId();
}

// ---- Input, sizes, colours. -------------------------------------------------

void RexSession::sendInput(Block *b, const QByteArray &bytes)
{
    // Raw bytes to the block's pty, as the emulator encoded them (the
    // snapshot gave it the modes: bracketed paste, mouse, keypad).
    if (b->exited || m_finished)
        return;
    takeBlock(b);
    for (qsizetype i = 0; i < bytes.size(); i += kWriteChunk) {
        QJsonObject p = target(b);
        p.insert(QStringLiteral("args"),
                 QJsonObject{{QStringLiteral("data"), QString::fromUtf8(bytes.mid(i, kWriteChunk))}});
        m_control->request(QStringLiteral("com.superlogical.terminal.write"), p);
    }
}

void RexSession::sendSize(Block *b, const QSize &cells, bool again)
{
    if (m_finished)
        return;
    const QSize cell = cellSize();
    // The same cells at a new cell size (a font change) is news too: the
    // server reports pixel sizes to programs from it.
    if (cells == b->sent && cell == b->sentCell && !again) {
        openStream(b);
        return;
    }
    qCDebug(lcRex) << "size" << b->id.right(8) << cells << "was" << b->sent << (again ? "again" : "");
    if (cells != b->sent)
        b->retries = 0;
    b->sent = cells;
    b->sentCell = cell;
    ++b->resizing;
    QJsonObject p = target(b);
    p.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("columns"), cells.width()},
                                                 {QStringLiteral("rows"), cells.height()},
                                                 {QStringLiteral("cell_width_px"), cell.width()},
                                                 {QStringLiteral("cell_height_px"), cell.height()}});
    const QString id = b->id;
    m_control->request(QStringLiteral("com.superlogical.terminal.resize"), p,
                       [this, id](const QJsonObject &, const QJsonObject &error) {
                           if (!error.isEmpty())
                               qCWarning(lcRex) << "resize refused:" << error;
                           auto it = m_blocks.find(id);
                           if (it == m_blocks.end())
                               return;
                           --it->second->resizing;
                           openStream(it->second.get());
                           checkSize(it->second.get());
                       });
}

void RexSession::checkSize(Block *b)
{
    // A server may not end up at the size last asked for (a resize can be
    // lost while it is busy, or another client may own the size): once the
    // stream has said what the block is and nothing is on its way, ask
    // again, a few times. The pane keeps the layout's size meanwhile.
    if (b->resizing > 0 || b->screen.isEmpty() || b->sent.isEmpty() || b->screen == b->sent)
        return;
    if (following(b))
        return;  // another client's size, until this one takes the block
    if (b->retries >= 3) {
        qCWarning(lcRex) << "block" << b->id << "stays" << b->screen << "not" << b->sent;
        return;
    }
    ++b->retries;
    const QString id = b->id;
    QTimer::singleShot(100 * b->retries, this, [this, id]() {
        auto it = m_blocks.find(id);
        if (it != m_blocks.end() && it->second->screen != it->second->sent && it->second->resizing == 0)
            sendSize(it->second.get(), it->second->sent, true);
    });
}

bool RexSession::isFollowing(const TerminalWidget *pane) const
{
    for (const auto &[id, b] : m_blocks)
        if (b->pane == pane)
            return following(b.get());
    return false;
}

bool RexSession::following(const Block *b) const
{
    return !b->owner.isEmpty() && b->owner != m_control->clientId();
}

void RexSession::setOwner(Block *b, const QString &owner)
{
    if (b->owner == owner)
        return;
    const bool was = following(b);
    b->owner = owner;
    qCDebug(lcRex) << "owner" << b->id.right(8) << owner.right(8) << (following(b) ? "following" : "");
    showFollowing(b);
    if (following(b) && b->pane && !b->screen.isEmpty())
        b->pane->setScreenSize(b->screen.width(), b->screen.height());
    // Ours again: the server applies the size this client last asked for;
    // check it did.
    if (was && !following(b)) {
        b->retries = 0;
        checkSize(b);
    }
}

void RexSession::showFollowing(Block *b)
{
    if (!b->pane)
        return;
    if (!following(b)) {
        b->pane->setFollowing({});
        b->pane->setToolTip({});
        return;
    }
    QString note = tr("Following another client");
    if (!b->screen.isEmpty())
        note = tr("Following another client \u00b7 %1\u00d7%2").arg(b->screen.width()).arg(b->screen.height());
    b->pane->setFollowing(note);
    b->pane->setToolTip(tr("Another client sizes this terminal. Type or click in it to take it over."));
}

void RexSession::takeBlock(Block *b)
{
    // Using a pane makes this client the block's owner: the block takes this
    // pane's size (session.own_block, take).
    if (!following(b) || b->taking || m_finished)
        return;
    b->taking = true;
    const QString id = b->id;
    m_control->request(QStringLiteral("session.own_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("block_ids"), QJsonArray{id}},
                                   {QStringLiteral("mode"), QStringLiteral("take")}},
                       [this, id](const QJsonObject &reply, const QJsonObject &error) {
                           auto it = m_blocks.find(id);
                           if (it == m_blocks.end())
                               return;
                           it->second->taking = false;
                           if (!error.isEmpty()) {
                               qCInfo(lcRex) << "own_block refused:" << error;
                               return;
                           }
                           for (const QJsonValue &v : reply.value(QLatin1String("blocks")).toArray())
                               if (str(v.toObject(), "block_id") == id)
                                   setOwner(it->second.get(), str(v.toObject(), "owner"));
                       });
}

void RexSession::requestBlocks()
{
    // Every block no one owns becomes this client's, and it joins the line
    // for the rest (session.own_block, request): it gets them when their
    // owners give them up or leave.
    m_control->request(QStringLiteral("session.own_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("mode"), QStringLiteral("request")}},
                       [this](const QJsonObject &reply, const QJsonObject &error) {
                           if (!error.isEmpty()) {
                               qCInfo(lcRex) << "own_block (request) refused:" << error;
                               return;
                           }
                           for (const QJsonValue &v : reply.value(QLatin1String("blocks")).toArray()) {
                               auto it = m_blocks.find(str(v.toObject(), "block_id"));
                               if (it != m_blocks.end())
                                   setOwner(it->second.get(), str(v.toObject(), "owner"));
                           }
                       });
}

void RexSession::sendTheme(Block *b)
{
    // The server answers programs' colour queries (OSC 10/11/4) itself, so
    // it should know the colours this pane shows.
    if (!b->pane)
        return;
    auto *display = b->pane->findChild<Konsole::TerminalDisplay *>();
    if (!display)
        return;
    const Konsole::ColorEntry *table = display->colorTable();
    QJsonArray palette;
    for (int i = 0; i < 8; ++i) {
        palette.append(QStringLiteral("%1=%2").arg(i).arg(colourName(table[2 + i].color).mid(1)));
        palette.append(QStringLiteral("%1=%2").arg(8 + i).arg(colourName(table[12 + i].color).mid(1)));
    }
    QJsonObject p = target(b);
    p.insert(QStringLiteral("args"),
             QJsonObject{{QStringLiteral("foreground"), colourName(table[0].color)},
                         {QStringLiteral("background"), colourName(b->pane->themeBackground())},
                         {QStringLiteral("palette"), palette}});
    m_control->request(QStringLiteral("com.superlogical.terminal.set_theme"), p,
                       [](const QJsonObject &, const QJsonObject &error) {
                           if (!error.isEmpty())
                               qCInfo(lcRex) << "set_theme refused:" << error;
                       });
}

void RexSession::splitterDragged(Window &w, TerminalWidget *before, Qt::Orientation orientation, int pixels)
{
    // The handle dragged follows `before`'s part. Where several splits start
    // with the same pane, it is the one whose part is nearest the size
    // dragged to. The server finds the split from a block on each side.
    const QSize cell = cellSize();
    const QSize chrome = paneChrome();
    const bool across = orientation == Qt::Horizontal;
    const QSize area = windowArea(w);
    int columns = qMax(10, (area.width() - chrome.width()) / qMax(1, cell.width()));
    int lines = qMax(3, (area.height() - chrome.height()) / qMax(1, cell.height()));
    // The layout the pane is in: the tiled one or a floating layer's.
    QJsonObject tree = w.layout;
    const QString layerId = w.tab ? w.tab->floatingLayerOf(before) : QString();
    for (const Window::Layer &l : std::as_const(w.floating)) {
        if (l.id != layerId)
            continue;
        QSize cells;
        floatingFrame(l.bounds, area, &cells);
        tree = l.layout;
        columns = cells.width();
        lines = cells.height();
    }
    QList<SplitInfo> splits;
    collectSplits(tree, columns, lines, across, splits);
    const QString beforeId = blockId(before);
    const int unit = qMax(1, across ? cell.width() : cell.height());
    const int cells = qMax(1, qRound(double(pixels - (across ? chrome.width() : chrome.height())) / unit));
    const SplitInfo *best = nullptr;
    for (const SplitInfo &s : std::as_const(splits)) {
        if (s.beforeBlock != beforeId)
            continue;
        if (!best || qAbs(s.beforeCells - cells) < qAbs(best->beforeCells - cells))
            best = &s;
    }
    if (!best || best->totalCells < 3)
        return;
    const double ratio = qBound(0.05, double(cells) / double(best->totalCells - 1), 0.95);
    m_control->request(QStringLiteral("session.resize_pane"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("before_block_id"), best->beforeBlock},
                                   {QStringLiteral("after_block_id"), best->afterBlock},
                                   {QStringLiteral("direction"), across ? QStringLiteral("horizontal")
                                                                        : QStringLiteral("vertical")},
                                   {QStringLiteral("ratio"), ratio}});
}

void RexSession::floatingMoved(Window &w, const QString &layerId, const QRect &frame)
{
    const QSize area = windowArea(w);
    if (area.isEmpty() || m_finished)
        return;
    for (const Window::Layer &l : std::as_const(w.floating)) {
        if (l.id != layerId)
            continue;
        // Moved, not resized: keep the bounds' size (the frame is only as
        // big as whole cells fill, so measuring it would shrink the layer).
        const QRect had = floatingFrame(l.bounds, area, nullptr);
        double width = frame.size() == had.size() ? l.bounds.width() : double(frame.width()) / area.width();
        double height = frame.size() == had.size() ? l.bounds.height() : double(frame.height()) / area.height();
        width = qBound(0.05, width, 1.0);
        height = qBound(0.05, height, 1.0);
        const double x = qBound(0.0, double(frame.x()) / area.width(), 1.0 - width);
        const double y = qBound(0.0, double(frame.y()) / area.height(), 1.0 - height);
        m_control->request(QStringLiteral("session.resize_layer"),
                           QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                       {QStringLiteral("layer_id"), layerId},
                                       {QStringLiteral("bounds"), rectJson(x, y, width, height)}},
                           [this](const QJsonObject &, const QJsonObject &error) {
                               // Refused: put the frame back where the view says.
                               if (!error.isEmpty()) {
                                   qCWarning(lcRex) << "resize_layer refused:" << error;
                                   for (Window &win : m_windows)
                                       layoutWindow(win);
                               }
                           });
        return;
    }
}

// ---- Actions. ---------------------------------------------------------------

void RexSession::toggleZoom(TerminalWidget *pane)
{
    const QString id = blockId(pane);
    if (id.isEmpty() || m_finished)
        return;
    takeBlock(block(id));  // used here: sized here
    m_control->request(QStringLiteral("session.zoom_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("block_id"), id}},
                       [](const QJsonObject &, const QJsonObject &error) {
                           if (!error.isEmpty())
                               qCWarning(lcRex) << "zoom refused:" << error;
                       });
}

bool RexSession::isZoomed(const TerminalTab *tab) const
{
    const Window *w = windowOf(tab);
    return w && (!w->zoomedBlock.isEmpty() || !w->tiledZoom.isEmpty());
}

bool RexSession::isZoomed(const TerminalWidget *pane) const
{
    const QString id = blockId(pane);
    if (id.isEmpty())
        return false;
    for (const Window &w : m_windows)
        if (w.zoomedBlock == id || w.tiledZoom == id)
            return true;
    return false;
}

void RexSession::swapWithNext(TerminalWidget *pane)
{
    const QString id = blockId(pane);
    if (id.isEmpty() || m_finished)
        return;
    for (const Window &w : std::as_const(m_windows)) {
        if (!w.tab || !w.tab->contains(pane))
            continue;
        const QList<TerminalWidget *> all = w.tab->panes();
        if (all.size() < 2)
            return;
        const QString other = blockId(all.at((all.indexOf(pane) + 1) % all.size()));
        m_control->request(QStringLiteral("session.swap_blocks"),
                           QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                       {QStringLiteral("block_id"), id},
                                       {QStringLiteral("other_block_id"), other}},
                           [](const QJsonObject &, const QJsonObject &error) {
                               if (!error.isEmpty())
                                   qCWarning(lcRex) << "swap refused:" << error;
                           });
        return;
    }
}

void RexSession::movePane(TerminalWidget *pane, TerminalTab *to)
{
    const QString id = blockId(pane);
    const Window *w = windowOf(to);
    if (id.isEmpty() || !w || m_finished || (w->tab && w->tab->contains(pane)))
        return;
    QStringList tiled;
    layoutBlocks(w->layout, tiled);
    if (tiled.isEmpty())
        return;
    const QString anchor = tiled.contains(w->focusedBlock) ? w->focusedBlock : tiled.first();
    m_control->request(QStringLiteral("session.move_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("block_id"), id},
                                   {QStringLiteral("anchor_block_id"), anchor},
                                   {QStringLiteral("direction"), QStringLiteral("horizontal")}},
                       [](const QJsonObject &, const QJsonObject &error) {
                           if (!error.isEmpty())
                               qCWarning(lcRex) << "move refused:" << error;
                       });
}

void RexSession::renameSession(const QString &name)
{
    if (m_finished)
        return;
    m_control->request(QStringLiteral("session.set_label"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("label"), name}});
}

void RexSession::windowShown(TerminalTab *tab)
{
    const Window *w = windowOf(tab);
    if (!w || m_finished || w->id == m_activeWindow)
        return;
    m_activeWindow = w->id;  // asked; the view will say
    m_control->request(QStringLiteral("session.focus_window"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("window_id"), w->id}});
}

bool RexSession::isFloating(const TerminalWidget *pane) const
{
    for (const Window &w : m_windows)
        if (w.tab && w.tab->contains(pane))
            return !w.tab->floatingLayerOf(pane).isEmpty();
    return false;
}

void RexSession::floatPane(TerminalWidget *pane)
{
    const QString id = blockId(pane);
    if (id.isEmpty() || isFloating(pane))
        return;
    takeBlock(block(id));
    // Where the pane is, unless that is (nearly) the whole window or too
    // small to use: then centred.
    QRectF bounds(0.2, 0.15, 0.6, 0.6);
    for (const Window &w : std::as_const(m_windows))
        if (w.tab && w.tab->contains(pane)) {
            const QRectF at = paneBounds(w.tab, pane);
            if (at.width() >= 0.15 && at.height() >= 0.15 && at.width() * at.height() < 0.8)
                bounds = at;
        }
    m_control->request(QStringLiteral("session.float_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("block_id"), id},
                                   {QStringLiteral("bounds"), rectJson(bounds.x(), bounds.y(), bounds.width(),
                                                                       bounds.height())}});
}

QRectF RexSession::paneBounds(const TerminalTab *tab, const TerminalWidget *pane)
{
    const QRect area = tab->layoutRect();
    if (area.isEmpty() || !pane->isVisibleTo(tab))
        return {};
    const QRect r(pane->mapTo(tab, QPoint(0, 0)), pane->size());
    const QRectF out((r.x() - area.x()) / double(area.width()), (r.y() - area.y()) / double(area.height()),
                     r.width() / double(area.width()), r.height() / double(area.height()));
    return out.intersected(QRectF(0, 0, 1, 1));
}

void RexSession::dockPane(TerminalWidget *pane)
{
    const QString id = blockId(pane);
    if (id.isEmpty() || !isFloating(pane))
        return;
    takeBlock(block(id));
    for (const Window &w : std::as_const(m_windows)) {
        if (!w.tab || !w.tab->contains(pane))
            continue;
        QStringList tiled;
        layoutBlocks(w.layout, tiled);
        if (tiled.isEmpty())
            return;  // nothing tiled to dock beside
        // Where the float's middle is: beside the tiled pane under it (or
        // the nearest), on the side of that pane it leans to.
        QPointF middle(0.5, 0.5);
        const QString layerId = w.tab->floatingLayerOf(pane);
        for (const Window::Layer &l : std::as_const(w.floating))
            if (l.id == layerId)
                middle = l.bounds.center();
        QString anchor = tiled.first();
        QRectF anchorRect;
        double best = -1;
        for (const QString &bid : std::as_const(tiled)) {
            const Block *tb = m_blocks.count(bid) ? m_blocks.at(bid).get() : nullptr;
            const QRectF r = tb && tb->pane ? paneBounds(w.tab, tb->pane) : QRectF();
            if (r.isEmpty())
                continue;  // hidden (a zoom)
            const double dx = qMax(0.0, qMax(r.left() - middle.x(), middle.x() - r.right()));
            const double dy = qMax(0.0, qMax(r.top() - middle.y(), middle.y() - r.bottom()));
            const double d = dx * dx + dy * dy;
            if (best < 0 || d < best) {
                best = d;
                anchor = bid;
                anchorRect = r;
            }
        }
        QString direction = QStringLiteral("horizontal"), side = QStringLiteral("after");
        if (!anchorRect.isEmpty()) {
            const double dx = (middle.x() - anchorRect.center().x()) / anchorRect.width();
            const double dy = (middle.y() - anchorRect.center().y()) / anchorRect.height();
            if (qAbs(dy) > qAbs(dx))
                direction = QStringLiteral("vertical");
            if ((direction == QLatin1String("vertical") ? dy : dx) < 0)
                side = QStringLiteral("before");
        }
        m_control->request(QStringLiteral("session.move_block"),
                           QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                       {QStringLiteral("block_id"), id},
                                       {QStringLiteral("anchor_block_id"), anchor},
                                       {QStringLiteral("direction"), direction},
                                       {QStringLiteral("side"), side}});
        return;
    }
}

void RexSession::newWindow()
{
    m_control->request(QStringLiteral("session.new_window"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("layout"), shellLayout()},
                                   {QStringLiteral("focus"), true}});
}

void RexSession::splitPane(TerminalWidget *pane, Qt::Orientation orientation)
{
    const QString id = blockId(pane);
    QString windowId;
    for (const Window &w : std::as_const(m_windows))
        if (w.tab && w.tab->contains(pane))
            windowId = w.id;
    if (id.isEmpty() || windowId.isEmpty())
        return;
    m_control->request(QStringLiteral("session.new_split"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("window_id"), windowId},
                                   {QStringLiteral("anchor_block_id"), id},
                                   {QStringLiteral("direction"), orientation == Qt::Horizontal
                                                                     ? QStringLiteral("horizontal")
                                                                     : QStringLiteral("vertical")},
                                   {QStringLiteral("layout"), shellLayout()},
                                   {QStringLiteral("focus"), true}});
}

void RexSession::closePane(TerminalWidget *pane)
{
    const QString id = blockId(pane);
    if (id.isEmpty())
        return;
    // Undoable (API revision 20): undoClose() brings it back for a while.
    m_lastUndoable.start();
    m_control->request(QStringLiteral("block.close"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("block_id"), id},
                                   {QStringLiteral("undoable"), true}});
}

void RexSession::undoClose(std::function<void(const QString &error)> done)
{
    m_control->request(QStringLiteral("client.undo_close"), QJsonObject{},
                       [done](const QJsonObject &, const QJsonObject &error) {
                           if (done)
                               done(error.isEmpty() ? QString() : str(error, "message"));
                       });
}

void RexSession::closeWindow(TerminalTab *tab)
{
    const Window *w = windowOf(tab);
    if (!w)
        return;
    m_lastUndoable.start();
    m_control->request(QStringLiteral("session.close_window"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId}, {QStringLiteral("window_id"), w->id},
                                   {QStringLiteral("undoable"), true}});
}

void RexSession::renameWindow(TerminalTab *tab, const QString &name)
{
    const Window *w = windowOf(tab);
    if (!w)
        return;
    m_control->request(QStringLiteral("session.set_window_label"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("window_id"), w->id},
                                   {QStringLiteral("label"), name}});
}

void RexSession::detach()
{
    if (m_finished)
        return;
    if (m_sessionId.isEmpty() || !m_control->isReady()) {
        finish(QString());
        return;
    }
    m_control->request(QStringLiteral("session.detach"), QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                       [this](const QJsonObject &, const QJsonObject &) { finish(tr("detached")); });
}

void RexSession::destroySession()
{
    if (m_finished)
        return;
    if (m_sessionId.isEmpty() || !m_control->isReady()) {
        finish(QString());
        return;
    }
    m_control->request(QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                       [this](const QJsonObject &, const QJsonObject &error) {
                           finish(error.isEmpty() ? tr("the session was ended") : str(error, "message"));
                       });
}

void RexSession::endIfEmpty()
{
    // Nothing left to show. A session with no blocks at all (a server may
    // keep one) is ended rather than left behind; one with floating blocks
    // is left be.
    m_control->request(QStringLiteral("session.view"), QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                       [this](const QJsonObject &view, const QJsonObject &error) {
                           // After an undoable close the server holds what
                           // closed for a while, and ends the session itself
                           // when the hold runs out unless it is undone.
                           const bool held = m_lastUndoable.isValid() && m_lastUndoable.elapsed() < 60000;
                           if (!error.isEmpty() || hasBlocks(view) || held) {
                               finish(error.isEmpty() ? tr("nothing left to show") : tr("the session ended"));
                               return;
                           }
                           m_control->request(QStringLiteral("session.destroy"),
                                              QJsonObject{{QStringLiteral("session_id"), m_sessionId}},
                                              [this](const QJsonObject &, const QJsonObject &) {
                                                  finish(tr("the session ended"));
                                              });
                       });
}

void RexSession::finish(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    qCInfo(lcRex) << "session" << m_sessionId << "ended:" << reason;
    for (auto &[id, b] : m_blocks)
        if (b->stream)
            b->stream->close();
    // Close the control connection once this has returned (we may be inside
    // one of its callbacks).
    QTimer::singleShot(0, m_control, &RexControl::close);
    for (const Window &w : std::exchange(m_windows, {}))
        if (w.tab)
            emit tabRemoved(w.tab);
    emit ended(reason);
}

void RexSession::listSessions(const QUrl &server, QObject *context,
                              std::function<void(const QList<QPair<QString, QString>> &, const QString &)> done)
{
    auto *control = new RexControl(context);
    auto finished = std::make_shared<bool>(false);
    auto reply = [control, finished, done](const QList<QPair<QString, QString>> &list, const QString &error) {
        if (std::exchange(*finished, true))
            return;
        done(list, error);
        control->disconnect();
        QTimer::singleShot(0, control, [control]() {
            control->close();
            control->deleteLater();
        });
    };
    QObject::connect(control, &RexControl::closed, control, [reply](const QString &reason) {
        reply({}, reason.isEmpty() ? QObject::tr("could not connect") : reason);
    });
    control->open(server);
    control->request(QStringLiteral("session.list"), {},
                     [reply](const QJsonObject &payload, const QJsonObject &error) {
                         if (!error.isEmpty()) {
                             reply({}, str(error, "message"));
                             return;
                         }
                         QList<QPair<QString, QString>> list;
                         for (const QJsonValue &v : payload.value(QLatin1String("sessions")).toArray()) {
                             const QJsonObject s = v.toObject();
                             const QString id = str(s, "session_id");
                             QString label = str(s, "label");
                             if (label.isEmpty())
                                 label = shortId(id);
                             list.append({id, label});
                         }
                         reply(list, QString());
                     });
}
