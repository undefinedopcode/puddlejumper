/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "tabstrip.h"

#include "cardstyle.h"

#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>

namespace {

constexpr int kTitleRole = Qt::UserRole + 1;
constexpr int kBusyRole = Qt::UserRole + 2;
constexpr int kNaturalWidthRole = Qt::UserRole + 3;   // before layoutTabs() narrows it

// Tab geometry, in logical pixels.
constexpr int kSide = 6;        // between the window edge and the first tab
constexpr int kGapH = 2;        // half the space between tabs
constexpr int kGapV = 4;        // above and below a tab
constexpr int kPadH = 10;       // inside a tab, before the title
constexpr int kPadV = 6;        // inside a tab, above and below the title
constexpr int kClose = 16;      // the close button
constexpr int kMinWidth = 110;
constexpr int kMaxWidth = 220;
constexpr int kFade = 28;       // the fade at an edge with tabs beyond it

using cardstyle::kDot;
using cardstyle::mix;

QFont titleFont(const QFont &base, bool selected)
{
    QFont f = base;
    f.setWeight(selected ? QFont::DemiBold : QFont::Normal);
    return f;
}

/*
 * Paints a tab: a card (cardstyle.h) with its title, the new-output dot and a
 * close button. The layout below is shared with hit-testing (closeRect).
 */
class TabDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QColor surface;
    QColor foreground;

    static QRect cardRect(const QRect &row)
    {
        return row.adjusted(kGapH, kGapV, -kGapH, -kGapV);
    }

    static QRect closeRect(const QRect &row)
    {
        const QRect card = cardRect(row);
        return QRect(card.right() - kPadH / 2 - kClose + 1, card.center().y() - kClose / 2 + 1,
                     kClose, kClose);
    }

    // The title's room: the card less its padding, the close button and the
    // dot. Both are always allowed for, so a title doesn't shift as the mouse
    // passes or output arrives.
    static QRect titleRect(const QRect &row)
    {
        const QRect card = cardRect(row);
        return QRect(card.left() + kPadH, card.top(),
                     closeRect(row).left() - 4 - kDot - 6 - card.left() - kPadH, card.height());
    }

    static int rowHeight(const QFont &font)
    {
        return 2 * kGapV + 2 * kPadV + QFontMetrics(font).height();
    }

    // The width a tab wants for its title, within kMinWidth..kMaxWidth.
    static int rowWidth(const QString &title, const QFont &font)
    {
        // Measured semibold, as the current tab's title is drawn, so a tab
        // doesn't change width on becoming current.
        const int text = QFontMetrics(titleFont(font, true)).horizontalAdvance(title);
        const int chrome = 2 * kGapH + kPadH + 6 + kDot + 4 + kClose + kPadH / 2;
        return qBound(kMinWidth, text + chrome, kMaxWidth);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        // Read each time, so a change of system accent shows at once.
        const QColor accent = cardstyle::accent();
        const QColor fill = cardstyle::paintCard(p, cardRect(option.rect), surface, foreground,
                                                 accent, hovered, selected);

        const QRect title = titleRect(option.rect);
        const QFont font = titleFont(option.font, selected);
        p->setFont(font);
        p->setPen(foreground);
        p->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(index.data(kTitleRole).toString(),
                                                  Qt::ElideRight, title.width()));

        if (index.data(kBusyRole).toBool())
            cardstyle::paintDot(p, QPointF(title.right() + 6 + kDot / 2.0,
                                           title.center().y() + 0.5), accent);

        // The close button: on the current tab and under the mouse, as a
        // tab bar shows it, so the others stay quiet.
        if (selected || hovered) {
            const QRect close = closeRect(option.rect);
            if (hovered) {
                p->setPen(Qt::NoPen);
                p->setBrush(mix(fill, foreground, 0.12));
                p->drawEllipse(close);
            }
            p->setPen(QPen(mix(foreground, fill, 0.25), 1.5, Qt::SolidLine, Qt::RoundCap));
            const QRectF x = QRectF(close).adjusted(5, 5, -5, -5);
            p->drawLine(x.topLeft(), x.bottomRight());
            p->drawLine(x.topRight(), x.bottomLeft());
        }

        p->restore();
    }
};

TabDelegate *delegateOf(QListWidget *list)
{
    return static_cast<TabDelegate *>(list->itemDelegate());
}

/*
 * The row of tabs. Its size hint is the width all its tabs want, so the strip
 * gives it that if it can and New Tab sits just after the last tab; given
 * less, the tabs narrow and then scroll (TabStrip::layoutTabs()). An edge
 * with tabs beyond it fades out.
 */
class TabList : public QListWidget
{
public:
    using QListWidget::QListWidget;

    QColor fade;   // the strip's surface, faded into at the edges

    QSize sizeHint() const override
    {
        // The natural widths, not the narrowed ones, or the row could never
        // grow back once narrowed.
        int width = 0;
        for (int i = 0; i < count(); ++i)
            width += item(i)->data(kNaturalWidthRole).toInt();
        return {width, height()};
    }

    QSize minimumSizeHint() const override { return {kMinWidth, height()}; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QListWidget::paintEvent(event);
        const QScrollBar *bar = horizontalScrollBar();
        if (bar->maximum() == 0)
            return;
        QPainter p(viewport());
        QColor clear = fade;
        clear.setAlpha(0);
        const int w = viewport()->width(), h = viewport()->height();
        if (bar->value() > 0) {
            QLinearGradient g(0, 0, kFade, 0);
            g.setColorAt(0, fade);
            g.setColorAt(1, clear);
            p.fillRect(0, 0, kFade, h, g);
        }
        if (bar->value() < bar->maximum()) {
            QLinearGradient g(w - kFade, 0, w, 0);
            g.setColorAt(0, clear);
            g.setColorAt(1, fade);
            p.fillRect(w - kFade, 0, kFade, h, g);
        }
    }
};

} // namespace

TabStrip::TabStrip(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(kSide - kGapH, 0, kSide, 0);
    row->setSpacing(2);

    m_list = new TabList(this);
    m_list->setItemDelegate(new TabDelegate(m_list));
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(false);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_list->setFixedHeight(TabDelegate::rowHeight(m_list->font()));
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover);
    m_list->viewport()->installEventFilter(this);
    m_list->setFocusPolicy(Qt::NoFocus);   // the terminal keeps the keyboard
    // Drag a tab to reorder.
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setDragDropOverwriteMode(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    row->addWidget(m_list);

    m_allTabs = new QToolButton(this);
    m_allTabs->setAutoRaise(true);
    m_allTabs->setToolTip(tr("All tabs"));
    m_allTabs->hide();
    row->addWidget(m_allTabs);

    m_newTab = new QToolButton(this);
    m_newTab->setAutoRaise(true);
    m_newTab->setToolTip(tr("Open a new tab"));
    row->addWidget(m_newTab);
    row->addStretch(1);

    connect(m_newTab, &QToolButton::clicked, this, &TabStrip::newTabRequested);
    connect(m_allTabs, &QToolButton::clicked, this, &TabStrip::showAllTabsMenu);
    // Scrolling blits what was already drawn, fades included, so redraw the
    // lot; and whether anything is out of sight changes with the range.
    QScrollBar *bar = m_list->horizontalScrollBar();
    connect(bar, &QScrollBar::valueChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    connect(bar, &QScrollBar::rangeChanged, this, &TabStrip::updateOverflow);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && !m_syncing)
            emit tabActivated(row);
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_list->indexAt(pos);
        emit contextMenuRequested(index.isValid() ? index.row() : -1,
                                  m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int start, int, const QModelIndex &, int row) {
                if (m_syncing)
                    return;
                const int to = row > start ? row - 1 : row;
                if (to != start)
                    emit tabMoved(start, to);
            });

    setColours(palette().color(QPalette::Base), palette().color(QPalette::Text));
}

QListWidgetItem *TabStrip::item(int index) const
{
    return (index >= 0 && index < m_list->count()) ? m_list->item(index) : nullptr;
}

void TabStrip::updateItemWidth(QListWidgetItem *item)
{
    item->setData(kNaturalWidthRole,
                  TabDelegate::rowWidth(item->data(kTitleRole).toString(), m_list->font()));
    m_list->updateGeometry();
    layoutTabs();
}

void TabStrip::layoutTabs()
{
    QList<int> natural;
    int total = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        natural << m_list->item(i)->data(kNaturalWidthRole).toInt();
        total += natural.last();
    }

    // Too wide: find the cap that makes them fit, narrowing only the tabs
    // wider than it, so a short title keeps its short tab.
    int cap = kMaxWidth;
    const int room = m_list->viewport()->width();
    if (total > room) {
        QList<int> sorted = natural;
        std::sort(sorted.begin(), sorted.end());
        int left = room;
        qsizetype rest = sorted.size();
        cap = kMinWidth;
        for (int w : std::as_const(sorted)) {
            if (w * rest <= left) {
                left -= w;
                --rest;
            } else {
                cap = qMax(kMinWidth, int(left / rest));
                break;
            }
        }
    }

    for (int i = 0; i < m_list->count(); ++i) {
        const QSize size(qMin(natural.at(i), cap), m_list->height());
        if (m_list->item(i)->sizeHint() != size)
            m_list->item(i)->setSizeHint(size);
    }
}

void TabStrip::updateOverflow()
{
    m_allTabs->setVisible(m_list->horizontalScrollBar()->maximum() > 0);
}

void TabStrip::showAllTabsMenu()
{
    QMenu menu(this);
    const QIcon dot = cardstyle::dotIcon(devicePixelRatioF());
    const QFontMetrics fm(menu.font());
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem *tab = m_list->item(i);
        QAction *act = menu.addAction(
            tab->data(kBusyRole).toBool() ? dot : QIcon(),
            fm.elidedText(tab->data(kTitleRole).toString(), Qt::ElideRight, 360));
        act->setCheckable(true);
        act->setChecked(i == m_list->currentRow());
        connect(act, &QAction::triggered, this, [this, i]() { emit tabActivated(i); });
    }
    menu.exec(m_allTabs->mapToGlobal(QPoint(0, m_allTabs->height())));
}

void TabStrip::syncTabs(const QStringList &titles, int currentIndex)
{
    // Programmatic changes must not look like the user clicking or dragging.
    m_syncing = true;
    while (m_list->count() > titles.size())
        delete m_list->takeItem(m_list->count() - 1);
    for (int i = 0; i < titles.size(); ++i) {
        if (!m_list->item(i)) {
            auto *added = new QListWidgetItem(m_list);
            added->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        setTitle(i, titles.at(i));
    }
    // Tabs only removed set no titles, so fit what's left here.
    m_list->updateGeometry();
    layoutTabs();
    if (currentIndex >= 0 && currentIndex < m_list->count())
        m_list->setCurrentRow(currentIndex);
    m_syncing = false;
}

void TabStrip::setCurrentIndex(int index)
{
    QListWidgetItem *current = item(index);
    if (!current)
        return;
    m_syncing = true;
    m_list->setCurrentRow(index);
    m_list->scrollToItem(current);
    m_syncing = false;
}

void TabStrip::setTitle(int index, const QString &title)
{
    if (QListWidgetItem *tab = item(index)) {
        const QString text = title.isEmpty() ? tr("Terminal") : title;
        tab->setData(kTitleRole, text);
        updateItemWidth(tab);
    }
}

void TabStrip::setTabToolTip(int index, const QString &toolTip)
{
    if (QListWidgetItem *tab = item(index))
        tab->setToolTip(toolTip);
}

void TabStrip::setBusy(int index, bool busy)
{
    if (QListWidgetItem *tab = item(index))
        tab->setData(kBusyRole, busy);
}

void TabStrip::moveTab(int from, int to)
{
    if (!item(from) || !item(to) || from == to)
        return;
    m_syncing = true;
    const int current = m_list->currentRow();
    QListWidgetItem *moved = m_list->takeItem(from);
    m_list->insertItem(to, moved);
    // Keep the same tab current: takeItem() moves the selection about.
    int now = current;
    if (current == from)
        now = to;
    else if (from < current && current <= to)
        --now;
    else if (to <= current && current < from)
        ++now;
    m_list->setCurrentRow(now);
    m_syncing = false;
}

void TabStrip::setColours(const QColor &background, const QColor &foreground)
{
    if (background == m_background && foreground == m_foreground)
        return;
    m_background = background;
    m_foreground = foreground;

    const QColor surface = cardstyle::surface(background, foreground);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, surface);
    pal.setColor(QPalette::Base, surface);
    pal.setColor(QPalette::Text, foreground);
    pal.setColor(QPalette::WindowText, foreground);
    pal.setColor(QPalette::ButtonText, foreground);
    pal.setColor(QPalette::Button, mix(surface, foreground, 0.08));
    setPalette(pal);
    m_list->setPalette(pal);
    m_newTab->setPalette(pal);
    m_newTab->setIcon(cardstyle::plusIcon(foreground, devicePixelRatioF()));
    m_allTabs->setPalette(pal);
    m_allTabs->setIcon(cardstyle::chevronIcon(foreground, devicePixelRatioF()));
    static_cast<TabList *>(m_list)->fade = surface;

    auto *delegate = delegateOf(m_list);
    delegate->surface = surface;
    delegate->foreground = foreground;
    m_list->viewport()->update();
}

void TabStrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    // The strip past the last tab, as for the list's own empty end below.
    if (event->button() == Qt::LeftButton)
        emit newTabRequested();
    else
        QWidget::mouseDoubleClickEvent(event);
}

bool TabStrip::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_list->viewport())
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Resize:
        layoutTabs();
        break;
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint pos = me->position().toPoint();
        const QModelIndex index = m_list->indexAt(pos);
        if (!index.isValid())
            break;
        // Middle click closes, as on a tab bar; so does the ✕.
        const bool onClose = me->button() == Qt::LeftButton
            && TabDelegate::closeRect(m_list->visualRect(index)).contains(pos);
        if (me->button() == Qt::MiddleButton || onClose) {
            emit tabClosed(index.row());
            return true;
        }
        break;
    }
    case QEvent::MouseButtonDblClick: {
        // Double-click the empty end of the strip for a new tab.
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && !m_list->indexAt(me->position().toPoint()).isValid()) {
            emit newTabRequested();
            return true;
        }
        break;
    }
    case QEvent::Wheel: {
        // The strip only scrolls sideways; let a vertical wheel do that.
        auto *we = static_cast<QWheelEvent *>(event);
        const QPoint delta = we->pixelDelta().isNull() ? we->angleDelta() / 2 : we->pixelDelta();
        QScrollBar *bar = m_list->horizontalScrollBar();
        bar->setValue(bar->value() - (delta.x() != 0 ? delta.x() : delta.y()));
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
