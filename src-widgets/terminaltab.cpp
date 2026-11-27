/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminaltab.h"

#include "appicon.h"
#include "cardstyle.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QSplitter>
#include <QSplitterHandle>
#include <QVBoxLayout>

#include <TerminalDisplay.h>

namespace {

// Handle width between panes in a local split. tmux layouts set their own,
// one cell wide (see TerminalTab::applyLayout()).
constexpr int kLocalHandleWidth = 4;

/*
 * Splitter handles painted in the terminal background, so the gap between
 * panes reads as part of the terminal rather than as window chrome (the style's
 * grip dots look out of place between two terminals).
 */
class PaneHandle : public QSplitterHandle
{
public:
    PaneHandle(Qt::Orientation orientation, QSplitter *parent, TerminalTab *tab)
        : QSplitterHandle(orientation, parent), m_tab(tab) {}

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_tab->containerColour());
    }

private:
    TerminalTab *m_tab;
};

class PaneSplitter : public QSplitter
{
public:
    PaneSplitter(Qt::Orientation orientation, TerminalTab *tab)
        : QSplitter(orientation), m_tab(tab) {}

protected:
    QSplitterHandle *createHandle() override
    {
        return new PaneHandle(orientation(), this, m_tab);
    }

private:
    TerminalTab *m_tab;
};

/* The structure of a layout without its sizes, to tell a resize from a
 * rearrangement. */
QString layoutShape(const PaneLayout &node)
{
    if (node.isLeaf())
        return QString::number(node.paneId);
    QString out = node.orientation == Qt::Horizontal ? QStringLiteral("{")
                                                     : QStringLiteral("[");
    for (const PaneLayout &child : node.children)
        out += layoutShape(child) + QLatin1Char(',');
    out += node.orientation == Qt::Horizontal ? QLatin1Char('}') : QLatin1Char(']');
    return out;
}

// A floating layer's frame: a rounded panel with a soft shadow and a clear
// edge (as the Rex prompts have), a title strip to move it by, a border to
// resize it by (right, bottom, or the corner), and its buttons. The shadow is
// drawn in a margin inside the frame's geometry, which is the layer's rect.
constexpr int kFloatShadow = 6;
constexpr int kFloatBorder = 4;   // the panel's edge round the content
constexpr int kFloatTitle = 22;
constexpr int kFloatRadius = 8;

// A small icon button for a frame's title strip or a pane's hover controls.
QToolButton *miniButton(QWidget *parent, int size)
{
    auto *b = new QToolButton(parent);
    b->setAutoRaise(true);
    b->setIconSize(QSize(size - 8, size - 8));
    b->setFixedSize(size, size);
    b->setCursor(Qt::ArrowCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

class FloatingFrame : public QWidget
{
public:
    FloatingFrame(TerminalTab *tab, const QString &id)
        : QWidget(tab)
        , m_tab(tab)
        , m_id(id)
    {
        setMouseTracking(true);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(kFloatShadow + kFloatBorder, kFloatShadow, kFloatShadow + kFloatBorder,
                              kFloatShadow + kFloatBorder);
        v->setSpacing(0);
        m_title = new QWidget(this);
        m_title->setFixedHeight(kFloatTitle);
        m_title->setCursor(Qt::SizeAllCursor);
        m_title->installEventFilter(this);
        auto *h = new QHBoxLayout(m_title);
        h->setContentsMargins(4, 0, 0, 0);
        h->setSpacing(2);
        m_label = new QLabel(m_title);
        QFont small = m_label->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        m_label->setFont(small);
        h->addWidget(m_label, 1);
        m_zoom = miniButton(m_title, kFloatTitle);
        QObject::connect(m_zoom, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = pane())
                emit m_tab->paneZoomRequested(p);
        });
        h->addWidget(m_zoom);
        m_dock = miniButton(m_title, kFloatTitle);
        m_dock->setToolTip(QObject::tr("Dock this pane back into the layout"));
        QObject::connect(m_dock, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = pane())
                emit m_tab->paneFloatRequested(p);
        });
        h->addWidget(m_dock);
        m_close = miniButton(m_title, kFloatTitle);
        m_close->setToolTip(QObject::tr("Close this floating pane"));
        QObject::connect(m_close, &QToolButton::clicked, this, [this]() {
            emit m_tab->floatingCloseRequested(m_id);
        });
        h->addWidget(m_close);
        setControls(false, false);
        v->addWidget(m_title);
    }

    void setContent(QWidget *content)
    {
        // Even the same widget: a rebuild lifts panes out of their frames, so
        // it may not be in this one any more.
        if (m_content)
            layout()->removeWidget(m_content);
        m_content = content;
        if (content) {
            static_cast<QVBoxLayout *>(layout())->addWidget(content, 1);
            content->show();
        }
    }

    void setActive(bool on)
    {
        if (on == m_active)
            return;
        m_active = on;
        update();
    }

    /* Zoom and dock buttons (for Rex layers), and whether its pane is zoomed. */
    void setControls(bool on, bool zoomed)
    {
        m_zoom->setVisible(on);
        m_dock->setVisible(on);
        m_zoomed = zoomed;
        m_zoom->setToolTip(zoomed ? QObject::tr("Unzoom this pane") : QObject::tr("Zoom this pane to fill the window"));
        updateIcons();
    }

    /* The pane the buttons act on: the active one if it is in this layer,
     * else the layer's first. */
    TerminalWidget *pane() const
    {
        if (!m_content)
            return nullptr;
        TerminalWidget *active = m_tab->activePane();
        if (active && (active == m_content || m_content->isAncestorOf(active)))
            return active;
        if (auto *p = qobject_cast<TerminalWidget *>(m_content.data()))
            return p;
        return m_content->findChild<TerminalWidget *>();
    }

    void setTitle(const QString &title)
    {
        m_label->setText(m_label->fontMetrics().elidedText(title, Qt::ElideRight,
                                                           qMax(20, width() - 2 * kFloatTitle)));
    }

    void setColours(const QColor &background, const QColor &foreground)
    {
        if (background == m_background)
            return;
        m_background = background;
        m_foreground = foreground;
        QPalette pal = m_label->palette();
        pal.setColor(QPalette::WindowText, foreground);
        m_label->setPalette(pal);
        updateIcons();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor bg = m_background.isValid() ? m_background : palette().color(QPalette::Window);
        const QColor fg = m_foreground.isValid() ? m_foreground : palette().color(QPalette::WindowText);
        const QRectF panel = QRectF(rect()).adjusted(kFloatShadow, kFloatShadow - 2, -kFloatShadow, -kFloatShadow);
        // A soft shadow, rings fading out from the panel and a little below it.
        for (int i = kFloatShadow; i > 0; --i) {
            QPainterPath ring;
            ring.addRoundedRect(panel.adjusted(-i, -i + 2, i, i + 2), kFloatRadius + i, kFloatRadius + i);
            const qreal t = 1.0 - qreal(i) / kFloatShadow;
            p.fillPath(ring, QColor(0, 0, 0, int(22 * t * t)));
        }
        QPainterPath path;
        path.addRoundedRect(panel, kFloatRadius, kFloatRadius);
        // Focused, with the focus tint: the frame round the (tinted) pane too.
        p.fillPath(path, bg);
        // The title strip, tinted towards the edge colour.
        const QColor edge = TerminalWidget::frameEdgeColour(bg, fg, m_active, true);
        p.save();
        p.setClipPath(path);
        p.fillRect(QRectF(panel.left(), panel.top(), panel.width(), kFloatTitle + 2),
                   cardstyle::mix(bg, edge, m_active ? 0.32 : 0.16));
        p.restore();
        // The edge, plain against any background: the accent when active.
        p.setPen(QPen(edge, m_active ? 2.0 : 1.5));
        p.drawPath(path);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_title)
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                begin(Move, me->globalPosition().toPoint());
            return false;
        }
        case QEvent::MouseMove:
            if (m_drag == Move)
                drag(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
            return false;
        case QEvent::MouseButtonRelease:
            if (m_drag == Move)
                finish();
            return false;
        default:
            return false;
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int e = edgesAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && e != None)
            begin(e, event->globalPosition().toPoint());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_drag != None) {
            drag(event->globalPosition().toPoint());
            return;
        }
        const int e = edgesAt(event->position().toPoint());
        setCursor(e == (Right | Bottom) ? Qt::SizeFDiagCursor
                  : e == Right          ? Qt::SizeHorCursor
                  : e == Bottom         ? Qt::SizeVerCursor
                                        : Qt::ArrowCursor);
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_drag != None)
            finish();
    }

private:
    enum { None = 0, Right = 1, Bottom = 2, Move = 4 };

    void updateIcons()
    {
        const QColor fg = m_foreground.isValid() ? m_foreground : palette().color(QPalette::WindowText);
        m_zoom->setIcon(appIcon(m_zoomed ? "minimize-2" : "maximize-2", fg));
        m_dock->setIcon(appIcon("picture-in-picture", fg));
        m_close->setIcon(appIcon("x", fg));
    }

    int edgesAt(const QPoint &pos) const
    {
        const int grab = kFloatShadow + kFloatBorder * 2;
        int e = None;
        if (pos.x() >= width() - grab)
            e |= Right;
        if (pos.y() >= height() - grab)
            e |= Bottom;
        return e;
    }

    void begin(int what, const QPoint &global)
    {
        m_drag = what;
        m_pressed = global;
        m_start = geometry();
        raise();
    }

    void drag(const QPoint &global)
    {
        const QPoint delta = global - m_pressed;
        const QRect area = m_tab->layoutRect();
        QRect r = m_start;
        if (m_drag == Move) {
            r.moveTopLeft(r.topLeft() + delta);
            r.moveLeft(qMax(area.left(), qMin(r.left(), area.right() - r.width() + 1)));
            r.moveTop(qMax(area.top(), qMin(r.top(), area.bottom() - r.height() + 1)));
        } else {
            const int minW = qMin(10 * kFloatTitle, area.width());
            const int minH = qMin(4 * kFloatTitle, area.height());
            if (m_drag & Right)
                r.setWidth(qMax(minW, qMin(m_start.width() + delta.x(), area.right() - r.left() + 1)));
            if (m_drag & Bottom)
                r.setHeight(qMax(minH, qMin(m_start.height() + delta.y(), area.bottom() - r.top() + 1)));
        }
        setGeometry(r);
    }

    void finish()
    {
        const bool moved = geometry() != m_start;
        m_drag = None;
        if (moved)
            emit m_tab->floatingGeometryChanged(m_id, geometry().translated(-m_tab->layoutRect().topLeft()));
    }

    TerminalTab *m_tab;
    QString m_id;
    QWidget *m_title = nullptr;
    QLabel *m_label = nullptr;
    QToolButton *m_zoom = nullptr;
    QToolButton *m_dock = nullptr;
    QToolButton *m_close = nullptr;
    QPointer<QWidget> m_content;
    QColor m_background;
    QColor m_foreground;
    bool m_active = false;
    bool m_zoomed = false;
    int m_drag = None;
    QPoint m_pressed;
    QRect m_start;
};

} // namespace

TerminalTab::~TerminalTab()
{
    // The panes are deleted after this, by QWidget's destructor, once this
    // class's members are gone: nothing of theirs may reach back here then
    // (their destroyed() went to a slot reading m_active, their events to
    // eventFilter(); a use after free that corrupted the heap, and crashed
    // later, closing a workspace of Rex tabs say).
    QList<TerminalWidget *> panes = findChildren<TerminalWidget *>();
    for (const QPointer<TerminalWidget> &p : std::as_const(m_parked))
        if (p && !panes.contains(p.data()))
            panes << p.data();
    for (TerminalWidget *pane : std::as_const(panes)) {
        pane->removeEventFilter(this);
        disconnect(pane, nullptr, this, nullptr);
    }
}

TerminalTab::TerminalTab(TerminalWidget *firstPane, QWidget *parent)
    : Tab(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    if (firstPane) {
        m_root = firstPane;
        m_layout->addWidget(firstPane);
        m_active = firstPane;
        watchPane(firstPane);
        updateFrames();
    }

    // The active pane follows keyboard focus, however it moved: a click, the
    // keyboard, or a pane being closed.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (auto *pane = paneContaining(now); pane && pane != m_active)
            setActivePane(pane, false);
    });
}

QList<TerminalWidget *> TerminalTab::panes() const
{
    QList<TerminalWidget *> out;
    collectPanes(m_root, out);
    for (const Floating &f : m_floating)
        collectPanes(f.root, out);
    return out;
}

QString TerminalTab::floatingLayerOf(const TerminalWidget *pane) const
{
    for (const Floating &f : m_floating) {
        QList<TerminalWidget *> in;
        collectPanes(f.root, in);
        if (in.contains(const_cast<TerminalWidget *>(pane)))
            return f.id;
    }
    return {};
}

QSize TerminalTab::paneGap(const QSize &cell, const QSize &chrome)
{
    if (TerminalWidget::paneCards() && chrome == TerminalWidget::externalChrome(true))
        return {kLocalHandleWidth, kLocalHandleWidth};  // as a local tab's
    return {qMax(1, cell.width() - chrome.width()), qMax(1, cell.height() - chrome.height())};
}

QSize TerminalTab::layoutOverflow(const PaneLayout &layout, const QSize &cell, const QSize &chrome)
{
    if (layout.isLeaf())
        return {};
    // Each gap past its separator cell, and below that each child's own.
    const QSize gap = paneGap(cell, chrome);
    const QSize extra = gap - (cell - chrome);
    const bool across = layout.orientation == Qt::Horizontal;
    QSize out;
    for (const PaneLayout &child : layout.children) {
        const QSize o = layoutOverflow(child, cell, chrome);
        if (across) {
            out.rwidth() += o.width();
            out.rheight() = qMax(out.height(), o.height());
        } else {
            out.rheight() += o.height();
            out.rwidth() = qMax(out.width(), o.width());
        }
    }
    const int separators = int(layout.children.size()) - 1;
    if (across)
        out.rwidth() += separators * extra.width();
    else
        out.rheight() += separators * extra.height();
    return out.expandedTo(QSize(0, 0));
}

QSize TerminalTab::floatingChrome()
{
    return QSize(2 * (kFloatShadow + kFloatBorder), 2 * kFloatShadow + kFloatTitle + kFloatBorder);
}

QRect TerminalTab::layoutRect() const
{
    return contentsRect().marginsRemoved(m_layout->contentsMargins());
}

bool TerminalTab::contains(const TerminalWidget *pane) const
{
    return pane && panes().contains(const_cast<TerminalWidget *>(pane));
}

void TerminalTab::collectPanes(QWidget *node, QList<TerminalWidget *> &out) const
{
    if (!node)
        return;
    if (auto *pane = qobject_cast<TerminalWidget *>(node)) {
        out << pane;
        return;
    }
    if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < splitter->count(); ++i)
            collectPanes(splitter->widget(i), out);
    }
}

TerminalWidget *TerminalTab::firstPaneIn(QWidget *node) const
{
    QList<TerminalWidget *> list;
    collectPanes(node, list);
    return list.value(0);
}

TerminalWidget *TerminalTab::paneContaining(QWidget *widget) const
{
    for (QWidget *w = widget; w && w != this; w = w->parentWidget()) {
        if (auto *pane = qobject_cast<TerminalWidget *>(w))
            return isAncestorOf(pane) ? pane : nullptr;
    }
    return nullptr;
}

QSplitter *TerminalTab::makeSplitter(Qt::Orientation orientation)
{
    auto *splitter = new PaneSplitter(orientation, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(kLocalHandleWidth);
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter](int, int index) {
        if (m_applyingLayout || index < 1)
            return;
        const QList<int> sizes = splitter->sizes();
        if (auto *before = firstPaneIn(splitter->widget(index - 1)))
            emit splitterDragged(before, splitter->orientation(), sizes.value(index - 1));
    });
    return splitter;
}

void TerminalTab::replaceInTree(QWidget *from, QWidget *to)
{
    if (auto *parentSplitter = qobject_cast<QSplitter *>(from->parentWidget());
        parentSplitter && parentSplitter != to) {
        const int index = parentSplitter->indexOf(from);
        const QList<int> sizes = parentSplitter->sizes();
        // Takes ownership of `to`; `from` loses its parent (and is hidden).
        parentSplitter->replaceWidget(index, to);
        parentSplitter->setSizes(sizes);
    } else {
        delete m_layout->replaceWidget(from, to);
        m_root = to;
    }
    to->show();
}

void TerminalTab::watchPane(TerminalWidget *pane)
{
    pane->installEventFilter(this);
    // A pane deleted from outside (its session ended) must not stay active.
    connect(pane, &QObject::destroyed, this, [this]() {
        if (!m_active)
            setActivePane(panes().value(0), false);
    });
}

void TerminalTab::splitPane(TerminalWidget *at, Qt::Orientation orientation,
                            TerminalWidget *pane)
{
    if (!at || !pane || !contains(at))
        return;

    auto *parentSplitter = qobject_cast<QSplitter *>(at->parentWidget());
    if (parentSplitter && parentSplitter->orientation() == orientation) {
        // Same direction as the existing split: one more sibling, taking half
        // of `at`'s space.
        const int index = parentSplitter->indexOf(at);
        QList<int> sizes = parentSplitter->sizes();
        const int half = sizes.value(index) / 2;
        sizes[index] -= half;
        sizes.insert(index + 1, half);
        parentSplitter->insertWidget(index + 1, pane);
        parentSplitter->setSizes(sizes);
    } else {
        // A new splitter in `at`'s place, holding `at` and the new pane.
        QSplitter *splitter = makeSplitter(orientation);
        replaceInTree(at, splitter);
        splitter->addWidget(at);
        splitter->addWidget(pane);
        at->show();
        splitter->setSizes({1, 1});   // proportions: equal halves
    }
    pane->show();
    watchPane(pane);
    emit paneAdded(pane);
    setActivePane(pane, true);
}

void TerminalTab::removePane(TerminalWidget *pane)
{
    // Floating layers are laid out by applyLayout() alone.
    if (!pane || !contains(pane) || !floatingLayerOf(pane).isEmpty())
        return;

    auto *parentSplitter = qobject_cast<QSplitter *>(pane->parentWidget());

    // The pane that takes over: a neighbour in the same splitter.
    TerminalWidget *next = nullptr;
    if (parentSplitter) {
        const int index = parentSplitter->indexOf(pane);
        QWidget *neighbour = parentSplitter->widget(index > 0 ? index - 1 : index + 1);
        next = firstPaneIn(neighbour);
    }

    const bool wasActive = (pane == m_active);
    pane->hide();
    pane->setParent(nullptr);   // also takes it out of the splitter
    pane->deleteLater();

    if (!parentSplitter) {
        m_root = nullptr;
        m_active = nullptr;
        emit empty();
        return;
    }
    if (parentSplitter->count() == 1)
        collapse(parentSplitter);

    if (wasActive || !m_active)
        setActivePane(next ? next : panes().value(0), true);
    else
        updateFrames();
}

void TerminalTab::collapse(QSplitter *splitter)
{
    QWidget *only = splitter->widget(0);
    replaceInTree(splitter, only);
    splitter->hide();
    splitter->deleteLater();
}

QJsonObject TerminalTab::saveLayout(const std::function<QJsonObject(TerminalWidget *)> &describe) const
{
    QJsonObject out = saveNode(m_root, describe);
    out.insert(QStringLiteral("active"), qMax(0, int(panes().indexOf(m_active.data()))));
    return out;
}

QJsonObject TerminalTab::saveNode(QWidget *node,
                                  const std::function<QJsonObject(TerminalWidget *)> &describe) const
{
    if (auto *pane = qobject_cast<TerminalWidget *>(node))
        return describe(pane);
    auto *splitter = qobject_cast<QSplitter *>(node);
    if (!splitter)
        return {};
    QJsonArray children;
    QJsonArray sizes;
    const QList<int> px = splitter->sizes();
    for (int i = 0; i < splitter->count(); ++i) {
        children.append(saveNode(splitter->widget(i), describe));
        sizes.append(px.value(i));
    }
    return {
        {QStringLiteral("split"), splitter->orientation() == Qt::Horizontal
                                      ? QStringLiteral("right") : QStringLiteral("down")},
        {QStringLiteral("sizes"), sizes},
        {QStringLiteral("panes"), children},
    };
}

TerminalTab *TerminalTab::restoreLayout(const QJsonObject &layout,
                                        const std::function<TerminalWidget *(const QJsonObject &)> &makePane)
{
    auto *tab = new TerminalTab(nullptr);
    QWidget *root = tab->restoreNode(layout, makePane);
    if (!root) {
        delete tab;
        return nullptr;
    }
    tab->m_root = root;
    tab->m_layout->addWidget(root);
    const QList<TerminalWidget *> all = tab->panes();
    for (TerminalWidget *pane : all)
        tab->watchPane(pane);
    tab->m_active = all.value(layout.value(QStringLiteral("active")).toInt(), all.value(0));
    tab->updateFrames();
    return tab;
}

QWidget *TerminalTab::restoreNode(const QJsonObject &node,
                                  const std::function<TerminalWidget *(const QJsonObject &)> &makePane)
{
    const QString split = node.value(QStringLiteral("split")).toString();
    if (split.isEmpty())
        return makePane(node);

    // A split that comes back with fewer than two parts is just that part.
    QList<QWidget *> parts;
    QList<int> sizes;
    const QJsonArray children = node.value(QStringLiteral("panes")).toArray();
    const QJsonArray savedSizes = node.value(QStringLiteral("sizes")).toArray();
    for (int i = 0; i < children.size(); ++i) {
        if (QWidget *part = restoreNode(children.at(i).toObject(), makePane)) {
            parts << part;
            sizes << qMax(1, savedSizes.at(i).toInt(1));
        }
    }
    if (parts.size() < 2)
        return parts.value(0);

    QSplitter *splitter = makeSplitter(split == QLatin1String("down") ? Qt::Vertical
                                                                      : Qt::Horizontal);
    for (QWidget *part : std::as_const(parts))
        splitter->addWidget(part);
    // Not shown yet, so these are taken as proportions when it is laid out.
    splitter->setSizes(sizes);
    return splitter;
}

void TerminalTab::setActivePane(TerminalWidget *pane, bool giveFocus)
{
    if (pane && !contains(pane))
        return;
    const bool changed = (pane != m_active);
    m_active = pane;
    updateFrames();
    if (pane && giveFocus)
        pane->focusTerminal();
    if (changed)
        emit activePaneChanged(pane);
    // A floating layer comes to the top when one of its panes is used
    // (even if it was the active pane already).
    if (pane && !m_applyingLayout && !m_floating.isEmpty()) {
        const QString layer = floatingLayerOf(pane);
        if (!layer.isEmpty() && m_floating.last().id != layer) {
            for (const Floating &f : std::as_const(m_floating))
                if (f.id == layer && f.frame)
                    f.frame->raise();
            emit floatingRaiseRequested(layer);
        }
    }
}

void TerminalTab::cyclePane(int step)
{
    const QList<TerminalWidget *> all = panes();
    if (all.size() < 2)
        return;
    const int index = qMax(0, all.indexOf(m_active.data()));
    setActivePane(all.at((index + step + all.size()) % all.size()), true);
}

void TerminalTab::setAlwaysFramed(bool on)
{
    m_alwaysFramed = on;
    updateFrames();
}

void TerminalTab::updateFrames()
{
    const QList<TerminalWidget *> all = panes();
    for (TerminalWidget *pane : all) {
        pane->setFloating(!floatingLayerOf(pane).isEmpty());
        if (all.size() == 1 && !m_alwaysFramed)
            pane->setPaneFrame(TerminalWidget::PaneFrame::None);
        else if (all.size() > 1 && pane == m_active)
            pane->setPaneFrame(TerminalWidget::PaneFrame::Active);
        else
            pane->setPaneFrame(TerminalWidget::PaneFrame::Inactive);
    }
    if (m_hover)
        m_hover->update();  // the hover buttons take the strip's colour
    const QString activeLayer = m_active ? floatingLayerOf(m_active) : QString();
    for (const Floating &f : std::as_const(m_floating)) {
        if (auto *frame = static_cast<FloatingFrame *>(f.frame.data())) {
            frame->setActive(f.id == activeLayer);
            frame->update();  // a quiet focus turned on or off
            frame->setColours(backgroundColour(), foregroundColour());
            frame->setControls(m_paneControls, isZoomedPane(frame->pane()));
            TerminalWidget *first = firstPaneIn(f.root);
            const QString title = first ? first->sessionTitle() : QString();
            frame->setTitle(!title.isEmpty() ? title : !f.title.isEmpty() ? f.title : tr("Terminal"));
        }
    }
}

QSize TerminalTab::layoutArea() const
{
    return layoutRect().size();
}

QColor TerminalTab::backgroundColour() const
{
    // The scheme's own, not the active pane's focus tint: the sidebar and
    // what's behind the panes don't change as the focus moves.
    if (TerminalWidget *pane = colourPane())
        return pane->themeBackground();
    return palette().color(QPalette::Window);
}

TerminalWidget *TerminalTab::colourPane() const
{
    // A pane given a scheme of its own (a Rex action) doesn't colour the
    // tab: the active pane, else the first, without one; with every pane
    // given one, the active pane after all.
    TerminalWidget *fallback = m_active ? m_active.data() : firstPaneIn(m_root);
    if (fallback && fallback->colorSchemeOverride().isEmpty())
        return fallback;
    for (TerminalWidget *pane : panes())
        if (pane->colorSchemeOverride().isEmpty())
            return pane;
    return fallback;
}

bool TerminalTab::hasNewOutput() const
{
    for (TerminalWidget *pane : panes()) {
        if (pane->hasNewOutput())
            return true;
    }
    return false;
}

void TerminalTab::clearNewOutput()
{
    for (TerminalWidget *pane : panes())
        pane->clearNewOutput();
}

void TerminalTab::focusContent()
{
    if (m_active)
        m_active->focusTerminal();
}

QColor TerminalTab::foregroundColour() const
{
    TerminalWidget *pane = colourPane();
    if (pane) {
        if (auto *display = pane->findChild<Konsole::TerminalDisplay *>())
            return display->colorTable()[DEFAULT_FORE_COLOR].color;
    }
    return palette().color(QPalette::WindowText);
}

QPixmap TerminalTab::previewPixmap(const QSize &size, qreal devicePixelRatio) const
{
    if (size.isEmpty() || !m_root)
        return {};
    QPixmap out((QSizeF(size) * devicePixelRatio).toSize());
    out.setDevicePixelRatio(devicePixelRatio);
    out.fill(backgroundColour());
    QPainter painter(&out);
    // Each pane's screen goes in its share of the box, top-left as on screen,
    // so a split tab previews as its arrangement. Drawn from the panes' screens,
    // which works for a hidden tab: no need to bring it to the front.
    paintPreview(painter, m_root, QRect(QPoint(0, 0), size), devicePixelRatio);
    // Floating layers over it, where they sit in the tab.
    const QRect area = layoutRect();
    if (!area.isEmpty()) {
        const double sx = double(size.width()) / area.width();
        const double sy = double(size.height()) / area.height();
        QColor edge = foregroundColour();
        edge.setAlphaF(0.5);
        for (const Floating &f : m_floating) {
            if (!f.frame || !f.root)
                continue;
            const QRect g = f.frame->geometry().translated(-area.topLeft());
            const QRect box(qRound(g.x() * sx), qRound(g.y() * sy), qRound(g.width() * sx),
                            qRound(g.height() * sy));
            painter.fillRect(box, backgroundColour());
            painter.setPen(edge);
            painter.drawRect(box.adjusted(0, 0, -1, -1));
            paintPreview(painter, f.root, box.adjusted(1, 1, -1, -1), devicePixelRatio);
        }
    }
    return out;
}

void TerminalTab::paintPreview(QPainter &painter, QWidget *node, const QRect &box,
                               qreal devicePixelRatio) const
{
    if (box.width() < 2 || box.height() < 2)
        return;
    if (auto *pane = qobject_cast<TerminalWidget *>(node)) {
        const QPixmap pm = pane->previewPixmap(box.size(), devicePixelRatio);
        if (!pm.isNull())
            painter.drawPixmap(box.topLeft(), pm);
        return;
    }
    auto *splitter = qobject_cast<QSplitter *>(node);
    if (!splitter || splitter->count() == 0)
        return;

    // Share the box as the splitter shares its space. A splitter in a tab that
    // has never been shown may not have sizes yet; split it evenly then.
    const bool across = splitter->orientation() == Qt::Horizontal;
    QList<int> sizes = splitter->sizes();
    qint64 total = 0;
    for (int s : sizes)
        total += qMax(0, s);
    if (total <= 0) {
        sizes = QList<int>(splitter->count(), 1);
        total = splitter->count();
    }

    constexpr int gap = 2;
    const int extent = (across ? box.width() : box.height()) - gap * (splitter->count() - 1);
    const QColor line = [this] {
        QColor c = foregroundColour();
        c.setAlphaF(0.25);
        return c;
    }();
    int pos = across ? box.left() : box.top();
    qint64 done = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        const int start = pos;
        done += qMax(0, sizes.value(i));
        const int end = (across ? box.left() : box.top()) + int(extent * done / total)
                        + gap * i;
        const QRect part = across ? QRect(start, box.top(), end - start, box.height())
                                  : QRect(box.left(), start, box.width(), end - start);
        paintPreview(painter, splitter->widget(i), part, devicePixelRatio);
        pos = end;
        if (i + 1 < splitter->count()) {
            const QRect divider = across ? QRect(pos, box.top(), gap, box.height())
                                         : QRect(box.left(), pos, box.width(), gap);
            painter.fillRect(divider, line);
            pos += gap;
        }
    }
}

void TerminalTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emit layoutAreaResized();
}

void TerminalTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    emit layoutAreaResized();
}

void TerminalTab::paintEvent(QPaintEvent *)
{
    // A server's layout is whole cells, which rarely fill the tab exactly:
    // what's left over (and the gaps) shows the terminal's background.
    if (!m_alwaysFramed)
        return;
    QPainter p(this);
    p.fillRect(rect(), containerColour());
}

QColor TerminalTab::containerColour() const
{
    // Pane cards sit on the sidebar's surface, so the window reads as one
    // piece; a lone unframed pane keeps the terminal's own background.
    for (TerminalWidget *pane : panes())
        if (pane->isCardShown())
            return cardstyle::surface(backgroundColour(), foregroundColour());
    return backgroundColour();
}

// ---- tmux layouts ------------------------------------------------------------

QWidget *TerminalTab::buildLayout(const PaneLayout &node,
                                  const std::function<TerminalWidget *(int)> &paneFor,
                                  QList<TerminalWidget *> &used)
{
    if (node.isLeaf()) {
        TerminalWidget *pane = paneFor(node.paneId);
        if (pane)
            used << pane;
        return pane;
    }
    QSplitter *splitter = makeSplitter(node.orientation);
    for (const PaneLayout &child : node.children) {
        if (QWidget *w = buildLayout(child, paneFor, used)) {
            splitter->addWidget(w);
            w->show();
        }
    }
    return splitter;
}

void TerminalTab::sizeLayout(QWidget *node, const PaneLayout &layout,
                             const QSize &cell, const QSize &chrome)
{
    if (layout.isLeaf()) {
        if (auto *pane = qobject_cast<TerminalWidget *>(node))
            pane->setScreenSize(layout.columns, layout.lines);
        return;
    }
    auto *splitter = qobject_cast<QSplitter *>(node);
    if (!splitter)
        return;

    // Every node is `cells * cell + chrome` pixels and every handle one cell
    // less the chrome, so a run of siblings adds up to exactly the pixels of
    // its parent's cells (the separator cell included). With pane cards the
    // handles are a fixed gap and a node takes its overflow on top, which
    // the client has kept free by asking for fewer cells.
    const bool across = layout.orientation == Qt::Horizontal;
    const QSize gap = paneGap(cell, chrome);
    splitter->setHandleWidth(across ? gap.width() : gap.height());
    QList<int> sizes;
    for (int i = 0; i < layout.children.size() && i < splitter->count(); ++i) {
        const PaneLayout &child = layout.children.at(i);
        const QSize over = layoutOverflow(child, cell, chrome);
        sizes << (across ? child.columns * cell.width() + chrome.width() + over.width()
                         : child.lines * cell.height() + chrome.height() + over.height());
        sizeLayout(splitter->widget(i), child, cell, chrome);
    }
    splitter->setSizes(sizes);
}

void TerminalTab::applyLayout(const PaneLayout &layout,
                              const std::function<TerminalWidget *(int)> &paneFor,
                              const QSize &cell, const QSize &chrome,
                              const QList<FloatingLayout> &floating,
                              const QList<int> &hidden)
{
    m_applyingLayout = true;

    const QString shape = layoutShape(layout);
    QString floatingShape;
    for (const FloatingLayout &f : floating)
        floatingShape += f.id + QLatin1Char(':') + layoutShape(f.layout) + QLatin1Char(';');
    for (int id : hidden)
        floatingShape += QStringLiteral("h%1;").arg(id);
    // (No tiled root is right when every pane floats: paneId -1.)
    const bool rootMissing = !m_root && !(layout.isLeaf() && layout.paneId < 0);
    if (shape != m_layoutShape || floatingShape != m_floatingShape || rootMissing) {
        // A different arrangement: rebuild the splitters and floating layers
        // together (a pane can move between them). Panes that stay are lifted
        // out first so deleting the old splitters doesn't take them.
        QList<TerminalWidget *> before = panes();
        for (const QPointer<TerminalWidget> &p : std::as_const(m_parked))
            if (p && !before.contains(p.data()))
                before << p.data();
        m_parked.clear();
        TerminalWidget *focused = paneContaining(QApplication::focusWidget());
        for (TerminalWidget *pane : before)
            pane->setParent(nullptr);
        if (m_root && !qobject_cast<TerminalWidget *>(m_root))
            m_root->deleteLater();
        m_root = nullptr;
        QHash<QString, FloatingFrame *> frames;
        for (const Floating &f : std::as_const(m_floating)) {
            if (f.root && !qobject_cast<TerminalWidget *>(f.root.data()))
                f.root->deleteLater();
            if (f.frame)
                frames.insert(f.id, static_cast<FloatingFrame *>(f.frame.data()));
        }
        m_floating.clear();

        QList<TerminalWidget *> used;
        QWidget *root = buildLayout(layout, paneFor, used);
        if (root) {
            m_layout->addWidget(root);
            root->show();
            m_root = root;
        }
        for (const FloatingLayout &f : floating) {
            FloatingFrame *frame = frames.take(f.id);
            if (!frame)
                frame = new FloatingFrame(this, f.id);
            QWidget *content = buildLayout(f.layout, paneFor, used);
            frame->setContent(content);
            frame->show();
            m_floating.append({f.id, frame, content, f.title});
        }
        for (FloatingFrame *frame : std::as_const(frames))
            frame->deleteLater();
        QList<TerminalWidget *> parked;
        for (int id : hidden) {
            if (TerminalWidget *pane = paneFor(id); pane && !used.contains(pane)) {
                pane->setParent(this);
                pane->hide();
                parked << pane;
                m_parked << pane;
                if (!before.contains(pane))
                    watchPane(pane);
            }
        }
        // Panes no longer here go, unless another tab takes them meanwhile
        // (a pane moved between windows is laid out there in the same turn).
        for (TerminalWidget *pane : before) {
            if (!used.contains(pane) && !parked.contains(pane)) {
                pane->hide();
                QTimer::singleShot(0, pane, [pane]() {
                    if (!pane->parentWidget())
                        pane->deleteLater();
                });
            }
        }
        for (TerminalWidget *pane : used) {
            pane->show();
            if (!before.contains(pane)) {
                watchPane(pane);
                emit paneAdded(pane);
            }
        }
        m_layoutShape = shape;
        m_floatingShape = floatingShape;

        if (!used.contains(m_active.data()))
            m_active = used.value(0);
        if (focused && used.contains(focused))
            focused->focusTerminal();
    }

    sizeLayout(m_root, layout, cell, chrome);
    const QRect area = layoutRect();
    const QSize extra = floatingChrome();
    // A floating pane's own frame is the plain one, whatever a tiled pane's
    // is (pane cards: the float's frame has the title).
    const QSize floatChrome = chrome == TerminalWidget::externalChrome(true) ? TerminalWidget::externalChrome(false) : chrome;
    for (int i = 0; i < m_floating.size() && i < floating.size(); ++i) {
        m_floating[i].title = floating.at(i).title;
        const Floating &f = m_floating.at(i);
        QList<TerminalWidget *> inLayer;
        collectPanes(f.root, inLayer);
        for (TerminalWidget *pane : std::as_const(inLayer))
            connect(pane, &TerminalWidget::titleChanged, this, &TerminalTab::updateFrames, Qt::UniqueConnection);
        const PaneLayout &l = floating.at(i).layout;
        sizeLayout(f.root, l, cell, floatChrome);
        if (!f.frame)
            continue;
        const QSize size(l.columns * cell.width() + floatChrome.width() + extra.width(),
                         l.lines * cell.height() + floatChrome.height() + extra.height());
        f.frame->setGeometry(QRect(area.topLeft() + floating.at(i).position, size));
        f.frame->raise();  // bottom to top, in order
    }
    updateFrames();
    m_applyingLayout = false;
}

namespace {

/*
 * Hover controls for tiled panes: a small pill of buttons in the top-right
 * corner of the pane under the mouse, to split it either way or close it,
 * and in Rex tabs to zoom or float it.
 */
class PaneControls : public QWidget
{
public:
    explicit PaneControls(TerminalTab *tab)
        : QWidget(tab), m_tab(tab)
    {
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(4, 3, 4, 3);
        h->setSpacing(2);
        m_right = miniButton(this, 22);
        m_down = miniButton(this, 22);
        m_zoom = miniButton(this, 22);
        m_float = miniButton(this, 22);
        m_close = miniButton(this, 22);
        for (QToolButton *b : {m_right, m_down, m_zoom, m_float, m_close})
            h->addWidget(b);
        // Each asks as the pane's own menu does, or as the tab's zoom and
        // float buttons do; whoever owns the pane decides what it means.
        QObject::connect(m_right, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = take())
                emit p->splitRequested(Qt::Horizontal);
        });
        QObject::connect(m_down, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = take())
                emit p->splitRequested(Qt::Vertical);
        });
        QObject::connect(m_close, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = take())
                emit p->closePaneRequested();
        });
        QObject::connect(m_zoom, &QToolButton::clicked, this, [this]() {
            if (m_pane)
                emit m_tab->paneZoomRequested(m_pane);
        });
        QObject::connect(m_float, &QToolButton::clicked, this, [this]() {
            if (TerminalWidget *p = take())
                emit m_tab->paneFloatRequested(p);
        });
        setCursor(Qt::ArrowCursor);
        hide();
    }

    TerminalWidget *pane() const { return m_pane; }

    void showFor(TerminalWidget *pane, bool zoomed, bool layoutButtons, const QColor &bg, const QColor &fg)
    {
        m_pane = pane;
        m_bg = bg;
        m_fg = fg;
        m_right->setIcon(appIcon("columns-2", fg));
        m_right->setToolTip(QObject::tr("Split right"));
        m_down->setIcon(appIcon("rows-2", fg));
        m_down->setToolTip(QObject::tr("Split down"));
        m_zoom->setIcon(appIcon(zoomed ? "minimize-2" : "maximize-2", fg));
        m_zoom->setToolTip(zoomed ? QObject::tr("Unzoom this pane") : QObject::tr("Zoom this pane to fill the window"));
        m_float->setIcon(appIcon("picture-in-picture-2", fg));
        m_float->setToolTip(QObject::tr("Float this pane over the window"));
        m_close->setIcon(appIcon("x", fg));
        m_close->setToolTip(QObject::tr("Close this pane"));
        m_zoom->setVisible(layoutButtons);
        m_float->setVisible(layoutButtons);
        setCompact(pane->isCardShown());
        adjustSize();
        place();
        raise();
        show();
        update();
    }

    /* In a card's title strip (pane cards), as a float's buttons are in
     * its title: small, flush with the strip. Else a pill over the pane. */
    void setCompact(bool on)
    {
        if (on == m_compact)
            return;
        m_compact = on;
        const int size = on ? TerminalWidget::cardTitleHeight() : 22;
        for (QToolButton *b : {m_right, m_down, m_zoom, m_float, m_close}) {
            b->setFixedSize(size, size);
            b->setIconSize(QSize(size - 8, size - 8));  // as miniButton()
        }
        layout()->setContentsMargins(on ? QMargins(4, 0, 4, 0) : QMargins(4, 3, 4, 3));
        layout()->setSpacing(2);
    }

    /* Into the pane's top-right corner, wherever the pane is now. */
    void place()
    {
        if (!m_pane)
            return;
        const QPoint corner = m_pane->mapTo(m_tab, QPoint(m_pane->width(), 0));
        QRect box(m_compact ? QPoint(corner.x() - width() - 3, corner.y())
                            : QPoint(corner.x() - width() - 8, corner.y() + 6),
                  size());
        // Not under a floating frame (one floated in place covers the
        // corner): left of it, or below it, while still on the pane.
        const QRect pane(m_pane->mapTo(m_tab, QPoint(0, 0)), m_pane->size());
        const QList<QRect> frames = m_tab->floatingFrameRects();
        for (int tries = 0; tries < 4; ++tries) {
            const auto hit = std::find_if(frames.cbegin(), frames.cend(),
                                          [&](const QRect &f) { return f.intersects(box); });
            if (hit == frames.cend())
                break;
            QRect left = box;
            left.moveRight(hit->left() - 8);
            QRect below = box;
            below.moveTop(hit->bottom() + 6);
            box = pane.contains(left) ? left : below;
        }
        if (pos() != box.topLeft())
            move(box.topLeft());
        raise();
    }

    void dismiss()
    {
        m_pane = nullptr;
        hide();
    }

    /* The pane, the controls going away (it may be about to go itself). */
    TerminalWidget *take()
    {
        TerminalWidget *p = m_pane;
        dismiss();
        return p;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        if (m_compact) {
            // Over the strip's title text, in the strip's own colour.
            path.addRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1), 4, 4);
            p.fillPath(path, m_pane ? m_pane->cardStripColour() : cardstyle::mix(m_bg, m_fg, 0.09));
            return;
        }
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        p.fillPath(path, cardstyle::mix(m_bg, m_fg, 0.12));
        p.setPen(QPen(cardstyle::mix(m_bg, m_fg, 0.40), 1.0));
        p.drawPath(path);
    }

    void leaveEvent(QEvent *) override
    {
        QTimer::singleShot(0, m_tab, [tab = QPointer<TerminalTab>(m_tab)]() {
            if (tab)
                tab->updateHoverControls();
        });
    }

private:
    TerminalTab *m_tab;
    QPointer<TerminalWidget> m_pane;
    QToolButton *m_right = nullptr;
    QToolButton *m_down = nullptr;
    QToolButton *m_zoom = nullptr;
    QToolButton *m_float = nullptr;
    QToolButton *m_close = nullptr;
    bool m_compact = false;
    QColor m_bg;
    QColor m_fg;
};

} // namespace

void TerminalTab::setPaneControls(bool on)
{
    m_paneControls = on;
    if (m_hover)
        static_cast<PaneControls *>(m_hover.data())->dismiss();  // shown again with its buttons
    updateFrames();
}

QList<QRect> TerminalTab::floatingFrameRects() const
{
    QList<QRect> out;
    for (const Floating &f : m_floating)
        if (f.frame && f.frame->isVisible())
            out << f.frame->geometry();
    return out;
}

void TerminalTab::setHoverControls(bool on)
{
    m_hoverControls = on;
    if (!on && m_hover)
        static_cast<PaneControls *>(m_hover.data())->dismiss();
}

void TerminalTab::setZoomedPanes(const QList<TerminalWidget *> &panes)
{
    QList<QPointer<TerminalWidget>> next;
    for (TerminalWidget *p : panes)
        next << p;
    if (next == m_zoomedPanes)
        return;
    m_zoomedPanes = next;
    updateFrames();
    if (auto *controls = static_cast<PaneControls *>(m_hover.data()))
        controls->dismiss();  // shown again with its new state
    updateHoverControls();
}

bool TerminalTab::isZoomedPane(const TerminalWidget *pane) const
{
    for (const QPointer<TerminalWidget> &p : m_zoomedPanes)
        if (p && p.data() == pane)
            return true;
    return false;
}

void TerminalTab::updateHoverControls()
{
    if (m_hoverControls && !m_hover)
        m_hover = new PaneControls(this);
    auto *controls = static_cast<PaneControls *>(m_hover.data());
    if (!controls)
        return;
    const QPoint global = QCursor::pos();
    if (!m_hoverControls || !isVisible() || QApplication::mouseButtons() != Qt::NoButton) {
        controls->dismiss();
        return;
    }
    // Over the controls themselves: they stay.
    if (controls->isVisible() && controls->rect().contains(controls->mapFromGlobal(global)))
        return;
    // A tiled pane under the mouse, not covered by a floating frame.
    for (const Floating &f : std::as_const(m_floating)) {
        if (f.frame && f.frame->isVisible() && f.frame->rect().contains(f.frame->mapFromGlobal(global))) {
            controls->dismiss();
            return;
        }
    }
    for (TerminalWidget *pane : panes()) {
        if (!pane->isVisible() || !floatingLayerOf(pane).isEmpty())
            continue;
        if (pane->rect().contains(pane->mapFromGlobal(global))) {
            if (controls->pane() != pane || !controls->isVisible())
                controls->showFor(pane, isZoomedPane(pane), m_paneControls, backgroundColour(), foregroundColour());
            else
                controls->place();  // the pane moved or changed size (a zoom)
            return;
        }
    }
    controls->dismiss();
}

bool TerminalTab::eventFilter(QObject *watched, QEvent *event)
{
    if (m_hoverControls && qobject_cast<TerminalWidget *>(watched)) {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Hide:
            QTimer::singleShot(0, this, &TerminalTab::updateHoverControls);
            break;
        default:
            break;
        }
    }
    return Tab::eventFilter(watched, event);
}
