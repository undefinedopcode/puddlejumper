/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "thumbnailsidebar.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kTitleRole = Qt::UserRole + 1;
constexpr int kSubtitleRole = Qt::UserRole + 2;
constexpr int kBadgeRole = Qt::UserRole + 3;
constexpr int kPreviewRole = Qt::UserRole + 4;
constexpr int kBusyRole = Qt::UserRole + 5;

// Card geometry, in logical pixels.
constexpr int kSide = 8;        // between the sidebar edge and a card
constexpr int kGap = 4;         // half the space between cards
constexpr int kPad = 6;         // inside a card, round the preview and text
constexpr int kRadius = 6;      // card corners
constexpr int kPreviewRadius = 3;
constexpr int kClose = 18;      // the close button on a hovered card
constexpr int kDot = 8;         // the new-output dot
constexpr int kScrollBar = 6;   // thin, and always reserved (see itemWidth())

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QFont smallFont(const QFont &base)
{
    QFont f = base;
    f.setPointSizeF(base.pointSizeF() * 0.85);
    return f;
}

/*
 * Paints the cards. Everything comes from the item's roles and the colours
 * the sidebar sets; the layout below is shared with hit-testing (closeRect).
 */
class CardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QColor background;
    QColor foreground;
    QColor accent;
    qreal aspect = 0.6;

    static QRect cardRect(const QRect &row)
    {
        return row.adjusted(kSide, kGap, -kSide, -kGap);
    }

    QRect previewRect(const QRect &row) const
    {
        const QRect card = cardRect(row);
        const int w = card.width() - 2 * kPad;
        return QRect(card.left() + kPad, card.top() + kPad, w, qRound(w * aspect));
    }

    QRect closeRect(const QRect &row) const
    {
        const QRect preview = previewRect(row);
        return QRect(preview.right() - 4 - kClose, preview.top() + 4, kClose, kClose);
    }

    int rowHeight(int previewHeight, const QFont &font) const
    {
        return 2 * kGap + kPad + previewHeight + 6 + QFontMetrics(font).height() + 1
               + QFontMetrics(smallFont(font)).height() + kPad;
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QRect card = cardRect(option.rect);

        // The card: lifted off the sidebar, more so under the mouse; the
        // current tab is tinted and outlined in the accent colour.
        QColor fill = mix(background, foreground, hovered ? 0.12 : 0.07);
        if (selected)
            fill = mix(fill, accent, 0.22);
        QPainterPath cardPath;
        cardPath.addRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
        p->fillPath(cardPath, fill);
        if (selected) {
            p->setPen(QPen(accent, 2));
            p->drawRoundedRect(QRectF(card).adjusted(1, 1, -1, -1), kRadius - 1, kRadius - 1);
        }

        // The preview, clipped to rounded corners.
        const QRect preview = previewRect(option.rect);
        QPainterPath previewPath;
        previewPath.addRoundedRect(QRectF(preview), kPreviewRadius, kPreviewRadius);
        p->save();
        p->setClipPath(previewPath);
        const QPixmap pm = index.data(kPreviewRole).value<QPixmap>();
        if (pm.isNull())
            p->fillRect(preview, background);
        else
            p->drawPixmap(preview.topLeft(), pm);
        p->restore();
        p->setPen(QPen(mix(fill, foreground, 0.15), 1));
        p->setBrush(Qt::NoBrush);
        p->drawPath(previewPath);

        // A badge on the preview (the tmux session).
        const QString badge = index.data(kBadgeRole).toString();
        const QFont small = smallFont(option.font);
        if (!badge.isEmpty()) {
            p->setFont(small);
            const QFontMetrics fm(small);
            const QString text = fm.elidedText(badge, Qt::ElideRight, preview.width() / 2);
            const QRect pill(preview.left() + 4, preview.bottom() - 4 - fm.height(),
                             fm.horizontalAdvance(text) + 10, fm.height());
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 150));
            p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p->setPen(Qt::white);
            p->drawText(pill, Qt::AlignCenter, text);
        }

        // Close button, only while hovered.
        if (hovered) {
            const QRect close = closeRect(option.rect);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 160));
            p->drawEllipse(close);
            p->setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
            const QRectF x = QRectF(close).adjusted(5.5, 5.5, -5.5, -5.5);
            p->drawLine(x.topLeft(), x.bottomRight());
            p->drawLine(x.topRight(), x.bottomLeft());
        }

        // Title, with the new-output dot at its end.
        const bool busy = index.data(kBusyRole).toBool();
        const QFontMetrics titleMetrics(option.font);
        QRect title(preview.left(), preview.bottom() + 1 + 6, preview.width(),
                    titleMetrics.height());
        if (busy) {
            const QRectF dot(title.right() - kDot, title.center().y() - kDot / 2.0 + 0.5,
                             kDot, kDot);
            p->setPen(Qt::NoPen);
            p->setBrush(accent);
            p->drawEllipse(dot);
            title.setRight(title.right() - kDot - 6);
        }
        QFont titleFont = option.font;
        titleFont.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        p->setFont(titleFont);
        p->setPen(foreground);
        p->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(index.data(kTitleRole).toString(),
                                                       Qt::ElideRight, title.width()));

        const QString subtitle = index.data(kSubtitleRole).toString();
        if (!subtitle.isEmpty()) {
            const QFontMetrics fm(small);
            const QRect sub(preview.left(), title.bottom() + 2, preview.width(), fm.height());
            p->setFont(small);
            p->setPen(mix(foreground, fill, 0.45));
            p->drawText(sub, Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(subtitle, Qt::ElideMiddle, sub.width()));
        }

        p->restore();
    }
};

CardDelegate *delegateOf(QListWidget *list)
{
    return static_cast<CardDelegate *>(list->itemDelegate());
}

} // namespace

ThumbnailSidebar::ThumbnailSidebar(QWidget *parent)
    : QWidget(parent)
{
    // No fixed width: the main window's splitter sets it, and remembers it.
    setMinimumWidth(140);
    setMaximumWidth(520);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setAutoFillBackground(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, kGap, 0, kGap);
    outer->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setUniformItemSizes(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover);
    m_list->viewport()->installEventFilter(this);
    m_list->setFocusPolicy(Qt::NoFocus);   // the terminal keeps the keyboard
    // Drag a card to reorder the tabs.
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setDragDropOverwriteMode(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    outer->addWidget(m_list, 1);

    m_newTab = new QToolButton(this);
    m_newTab->setText(tr("New Tab"));
    m_newTab->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_newTab->setAutoRaise(true);
    m_newTab->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_newTab->setToolTip(tr("Open a new tab"));
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(kSide, kGap, kSide, 0);
    footer->addWidget(m_newTab);
    outer->addLayout(footer);

    connect(m_newTab, &QToolButton::clicked, this, &ThumbnailSidebar::newTabRequested);
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

QListWidgetItem *ThumbnailSidebar::card(int index) const
{
    return (index >= 0 && index < m_list->count()) ? m_list->item(index) : nullptr;
}

void ThumbnailSidebar::syncTabs(const QStringList &titles, int currentIndex)
{
    // Programmatic changes must not look like the user clicking or dragging.
    m_syncing = true;
    while (m_list->count() > titles.size())
        delete m_list->takeItem(m_list->count() - 1);
    for (int i = 0; i < titles.size(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (!item) {
            item = new QListWidgetItem(m_list);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        }
        setTitle(i, titles.at(i));
    }
    updateItemSizes();
    if (currentIndex >= 0 && currentIndex < m_list->count())
        m_list->setCurrentRow(currentIndex);
    m_syncing = false;
}

void ThumbnailSidebar::setCurrentIndex(int index)
{
    if (!card(index))
        return;
    m_syncing = true;
    m_list->setCurrentRow(index);
    m_syncing = false;
}

void ThumbnailSidebar::setTitle(int index, const QString &title)
{
    if (QListWidgetItem *item = card(index)) {
        const QString text = title.isEmpty() ? tr("Terminal") : title;
        item->setData(kTitleRole, text);
        item->setToolTip(text);
    }
}

void ThumbnailSidebar::setSubtitle(int index, const QString &subtitle)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kSubtitleRole, subtitle);
}

void ThumbnailSidebar::setBadge(int index, const QString &badge)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kBadgeRole, badge);
}

void ThumbnailSidebar::setPreview(int index, const QPixmap &pixmap)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kPreviewRole, pixmap);
}

void ThumbnailSidebar::setBusy(int index, bool busy)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kBusyRole, busy);
}

void ThumbnailSidebar::moveCard(int from, int to)
{
    if (!card(from) || !card(to) || from == to)
        return;
    m_syncing = true;
    const int current = m_list->currentRow();
    QListWidgetItem *item = m_list->takeItem(from);
    m_list->insertItem(to, item);
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

void ThumbnailSidebar::setPreviewAspect(qreal aspect)
{
    aspect = qBound(0.3, aspect, 1.2);
    if (qFuzzyCompare(aspect, m_aspect))
        return;
    m_aspect = aspect;
    delegateOf(m_list)->aspect = aspect;
    updateItemSizes();
}

QSize ThumbnailSidebar::previewSize() const
{
    // Cards span the list, less the margins and a thin scroll bar. The scroll
    // bar's room is kept whether or not it shows, so cards don't change size
    // (and previews needn't be redrawn) as tabs come and go.
    const int w = qMax(40, m_list->width() - kScrollBar - 2 * kSide - 2 * kPad);
    return QSize(w, qRound(w * m_aspect));
}

void ThumbnailSidebar::setColours(const QColor &background, const QColor &foreground)
{
    if (background == m_background && foreground == m_foreground)
        return;
    m_background = background;
    m_foreground = foreground;

    // Lifted a little off the terminal, so the two read as one surface with
    // a seam rather than as unrelated widgets.
    const QColor side = mix(background, foreground, 0.04);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, side);
    pal.setColor(QPalette::Base, side);
    pal.setColor(QPalette::Text, foreground);
    pal.setColor(QPalette::WindowText, foreground);
    pal.setColor(QPalette::ButtonText, foreground);
    pal.setColor(QPalette::Button, mix(side, foreground, 0.08));
    setPalette(pal);
    m_list->setPalette(pal);
    m_newTab->setPalette(pal);

    // A plain plus in the text colour: theme icons are drawn for the window
    // colours, and a dark one vanishes on a dark scheme.
    const qreal dpr = devicePixelRatioF();
    QPixmap plus(QSize(16, 16) * dpr);
    plus.setDevicePixelRatio(dpr);
    plus.fill(Qt::transparent);
    {
        QPainter p(&plus);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(foreground, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 3), QPointF(8, 13));
        p.drawLine(QPointF(3, 8), QPointF(13, 8));
    }
    m_newTab->setIcon(QIcon(plus));

    auto *delegate = delegateOf(m_list);
    delegate->background = side;
    delegate->foreground = foreground;
    delegate->accent = QApplication::palette().color(QPalette::Highlight);

    const QColor handle = mix(side, foreground, 0.25);
    m_list->verticalScrollBar()->setStyleSheet(
        QStringLiteral("QScrollBar:vertical { background: transparent; width: %1px; margin: 0; }"
                       "QScrollBar::handle:vertical { background: %2; border-radius: %3px;"
                       " min-height: 24px; }"
                       "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }"
                       "QScrollBar::add-page, QScrollBar::sub-page { background: none; }")
            .arg(kScrollBar)
            .arg(handle.name())
            .arg(kScrollBar / 2));
    m_list->viewport()->update();
}

void ThumbnailSidebar::updateItemSizes()
{
    const QSize preview = previewSize();
    const int width = m_list->width() - kScrollBar;
    const int height = delegateOf(m_list)->rowHeight(preview.height(), m_list->font());
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setSizeHint(QSize(width, height));
    m_list->doItemsLayout();
    if (preview != m_lastPreviewSize) {
        m_lastPreviewSize = preview;
        emit previewSizeChanged();
    }
}

void ThumbnailSidebar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateItemSizes();
}

bool ThumbnailSidebar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_list->indexAt(me->position().toPoint());
        if (index.isValid()) {
            // Middle click closes, as on a tab bar; so does the hover ✕.
            const bool onClose = me->button() == Qt::LeftButton
                && delegateOf(m_list)->closeRect(m_list->visualRect(index))
                       .contains(me->position().toPoint());
            if (me->button() == Qt::MiddleButton || onClose) {
                emit tabClosed(index.row());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
