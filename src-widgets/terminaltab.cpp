/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminaltab.h"

#include "appicon.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QJsonArray>
#include <QPainter>
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
        p.fillRect(rect(), m_tab->backgroundColour());
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

// A floating layer's frame: a title strip to move it by, a border to resize
// it by (right, bottom, or the corner), and a close button.
constexpr int kFloatBorder = 3;
constexpr int kFloatTitle = 18;

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
        v->setContentsMargins(kFloatBorder, 0, kFloatBorder, kFloatBorder);
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
        auto *close = new QToolButton(m_title);
        close->setAutoRaise(true);
        close->setIcon(appIcon("x"));
        close->setIconSize(QSize(kFloatTitle - 6, kFloatTitle - 6));
        close->setFixedSize(kFloatTitle, kFloatTitle);
        close->setCursor(Qt::ArrowCursor);
        close->setToolTip(QObject::tr("Close this floating pane"));
        QObject::connect(close, &QToolButton::clicked, this, [this]() {
            emit m_tab->floatingCloseRequested(m_id);
        });
        h->addWidget(close);
        v->addWidget(m_title);
    }

    void setContent(QWidget *content)
    {
        if (content == m_content)
            return;
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
        QPalette pal = m_label->palette();
        pal.setColor(QPalette::WindowText, foreground);
        m_label->setPalette(pal);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QColor edge = m_active ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid);
        p.fillRect(rect(), m_background.isValid() ? m_background : palette().color(QPalette::Window));
        // The title strip a shade of the frame colour over the terminal's
        // background, so it reads as part of the layer.
        QColor strip = edge;
        strip.setAlphaF(m_active ? 0.55 : 0.35);
        p.fillRect(QRect(0, 0, width(), kFloatTitle), strip);
        p.setPen(QPen(edge, 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
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

    int edgesAt(const QPoint &pos) const
    {
        const int grab = kFloatBorder * 2;
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
    QPointer<QWidget> m_content;
    QColor m_background;
    bool m_active = false;
    int m_drag = None;
    QPoint m_pressed;
    QRect m_start;
};

} // namespace

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

QSize TerminalTab::floatingChrome()
{
    return QSize(2 * kFloatBorder, kFloatTitle + kFloatBorder);
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
    // A floating layer comes to the top when one of its panes is used.
    if (changed && pane && !m_applyingLayout) {
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
        if (all.size() == 1 && !m_alwaysFramed)
            pane->setPaneFrame(TerminalWidget::PaneFrame::None);
        else if (all.size() > 1 && pane == m_active)
            pane->setPaneFrame(TerminalWidget::PaneFrame::Active);
        else
            pane->setPaneFrame(TerminalWidget::PaneFrame::Inactive);
    }
    const QString activeLayer = m_active ? floatingLayerOf(m_active) : QString();
    for (const Floating &f : std::as_const(m_floating)) {
        if (auto *frame = static_cast<FloatingFrame *>(f.frame.data())) {
            frame->setActive(f.id == activeLayer);
            frame->setColours(backgroundColour(), foregroundColour());
            if (TerminalWidget *first = firstPaneIn(f.root))
                frame->setTitle(first->sessionTitle());
        }
    }
}

void TerminalTab::setPadding(int pixels)
{
    m_padding = qMax(0, pixels);
    m_layout->setContentsMargins(m_padding, m_padding, m_padding, m_padding);
    update();
}

QSize TerminalTab::layoutArea() const
{
    return layoutRect().size();
}

QColor TerminalTab::backgroundColour() const
{
    TerminalWidget *pane = m_active ? m_active.data() : firstPaneIn(m_root);
    if (pane) {
        if (auto *display = pane->findChild<Konsole::TerminalDisplay *>())
            return display->palette().color(QPalette::Window);
    }
    return palette().color(QPalette::Window);
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
    TerminalWidget *pane = m_active ? m_active.data() : firstPaneIn(m_root);
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
    if (m_padding == 0)
        return;
    QPainter p(this);
    p.fillRect(rect(), backgroundColour());
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
    // its parent's cells (the separator cell included).
    const bool across = layout.orientation == Qt::Horizontal;
    splitter->setHandleWidth(qMax(1, across ? cell.width() - chrome.width()
                                            : cell.height() - chrome.height()));
    QList<int> sizes;
    for (int i = 0; i < layout.children.size() && i < splitter->count(); ++i) {
        const PaneLayout &child = layout.children.at(i);
        sizes << (across ? child.columns * cell.width() + chrome.width()
                         : child.lines * cell.height() + chrome.height());
        sizeLayout(splitter->widget(i), child, cell, chrome);
    }
    splitter->setSizes(sizes);
}

void TerminalTab::applyLayout(const PaneLayout &layout,
                              const std::function<TerminalWidget *(int)> &paneFor,
                              const QSize &cell, const QSize &chrome,
                              const QList<FloatingLayout> &floating)
{
    m_applyingLayout = true;

    const QString shape = layoutShape(layout);
    QString floatingShape;
    for (const FloatingLayout &f : floating)
        floatingShape += f.id + QLatin1Char(':') + layoutShape(f.layout) + QLatin1Char(';');
    // (No tiled root is right when every pane floats: paneId -1.)
    const bool rootMissing = !m_root && !(layout.isLeaf() && layout.paneId < 0);
    if (shape != m_layoutShape || floatingShape != m_floatingShape || rootMissing) {
        // A different arrangement: rebuild the splitters and floating layers
        // together (a pane can move between them). Panes that stay are lifted
        // out first so deleting the old splitters doesn't take them.
        const QList<TerminalWidget *> before = panes();
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
            m_floating.append({f.id, frame, content});
        }
        for (FloatingFrame *frame : std::as_const(frames))
            frame->deleteLater();
        for (TerminalWidget *pane : before) {
            if (!used.contains(pane))
                pane->deleteLater();
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
    for (int i = 0; i < m_floating.size() && i < floating.size(); ++i) {
        const Floating &f = m_floating.at(i);
        const PaneLayout &l = floating.at(i).layout;
        sizeLayout(f.root, l, cell, chrome);
        if (!f.frame)
            continue;
        const QSize size(l.columns * cell.width() + chrome.width() + extra.width(),
                         l.lines * cell.height() + chrome.height() + extra.height());
        f.frame->setGeometry(QRect(area.topLeft() + floating.at(i).position, size));
        f.frame->raise();  // bottom to top, in order
    }
    updateFrames();
    m_applyingLayout = false;
}
