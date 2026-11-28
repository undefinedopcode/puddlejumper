/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "thumbnailsidebar.h"

#include "appicon.h"

#include "cardstyle.h"
#include "flattoolbutton.h"

#include <QAbstractItemModel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kTitleRole = Qt::UserRole + 1;
constexpr int kSubtitleRole = Qt::UserRole + 2;
constexpr int kBadgeRole = Qt::UserRole + 3;
constexpr int kPreviewRole = Qt::UserRole + 4;
constexpr int kBusyRole = Qt::UserRole + 5;
constexpr int kStatusKindRole = Qt::UserRole + 6;
constexpr int kStatusLabelRole = Qt::UserRole + 7;
constexpr int kStatusProgressRole = Qt::UserRole + 8;
constexpr int kStatusDetailRole = Qt::UserRole + 9;
constexpr int kIconRole = Qt::UserRole + 10;   // appIcon() name: the tab's protocol
constexpr int kTabRole = Qt::UserRole + 11;    // a card's tab index
constexpr int kGroupRole = Qt::UserRole + 12;  // a card's group; a header's own
constexpr int kGroupLabelRole = Qt::UserRole + 13;
constexpr int kGroupIconRole = Qt::UserRole + 14;
constexpr int kHeaderRole = Qt::UserRole + 15; // true on a group's header
constexpr int kCountRole = Qt::UserRole + 16;  // a header's number of cards
constexpr int kCollapsedRole = Qt::UserRole + 17;
constexpr int kBlockedCountRole = Qt::UserRole + 18;  // a header's cards waiting on the user
constexpr int kAnyBusyRole = Qt::UserRole + 19;       // a header's cards with unseen output

// Card geometry, in logical pixels.
constexpr int kSide = 8;        // between the sidebar edge and a card
constexpr int kGap = 4;         // half the space between cards
constexpr int kPad = 6;         // inside a card, round the preview and text
constexpr int kPreviewRadius = 3;
constexpr int kClose = 18;      // the close button on a hovered card
constexpr int kScrollBar = 6;   // thin, and always reserved (see itemWidth())

// Compact cards (docs/compact-sidebar.md): a thumbnail at the left, the
// details beside it.
constexpr int kThumbHeight = 40;
constexpr int kThumbMinWidth = 48;
constexpr int kThumbMaxWidth = 80;
constexpr int kNarrow = 220;         // a sidebar narrower crops its thumbnails
constexpr int kMinWidth = 140;
constexpr int kCompactMinWidth = 180;

// A compact thumbnail's natural size, for the terminal area's shape.
QSize thumbSize(qreal aspect)
{
    return QSize(qBound(kThumbMinWidth, qRound(kThumbHeight / aspect), kThumbMaxWidth), kThumbHeight);
}

using cardstyle::kDot;
using cardstyle::mix;

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
    qreal aspect = 0.6;
    bool tintBlocked = false;
    bool compact = false;

    static QRect cardRect(const QRect &row)
    {
        return row.adjusted(kSide, kGap, -kSide, -kGap);
    }

    QRect previewRect(const QRect &row) const
    {
        const QRect card = cardRect(row);
        if (compact) {
            // In a narrow sidebar the thumbnail is cropped to its least
            // width (its preview is still drawn at its natural size).
            QSize size = thumbSize(aspect);
            if (row.width() + kScrollBar < kNarrow)
                size.setWidth(kThumbMinWidth);
            return QRect(QPoint(card.left() + kPad, card.center().y() - size.height() / 2 + 1), size);
        }
        const int w = card.width() - 2 * kPad;
        return QRect(card.left() + kPad, card.top() + kPad, w, qRound(w * aspect));
    }

    QRect closeRect(const QRect &row) const
    {
        if (compact) {
            // At the card's end, clear of the thumbnail.
            const QRect card = cardRect(row);
            return QRect(card.right() - kPad - kClose + 1, card.center().y() - kClose / 2 + 1, kClose, kClose);
        }
        const QRect preview = previewRect(row);
        return QRect(preview.right() - 4 - kClose, preview.top() + 4, kClose, kClose);
    }

    int rowHeight(int previewHeight, const QFont &font) const
    {
        if (compact) {
            const int text = QFontMetrics(font).height() + 2 + QFontMetrics(smallFont(font)).height();
            return std::max(kThumbHeight, text) + 2 * kPad + 2 * kGap;
        }
        return 2 * kGap + kPad + previewHeight + 6 + QFontMetrics(font).height() + 1
               + QFontMetrics(smallFont(font)).height() + kPad;
    }

    // The tab's protocol icon at the start of `title`, a little dimmer
    // than the text unless the card is current; the title's room after it.
    QRect paintIcon(QPainter *p, const QModelIndex &index, QRect title, int lineHeight, bool selected) const
    {
        const QString icon = index.data(kIconRole).toString();
        if (icon.isEmpty())
            return title;
        const int side = lineHeight - 3;
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const QPixmap pm = appIcon(icon.toLatin1().constData(), foreground).pixmap(QSize(side, side), dpr);
        p->setOpacity(selected ? 0.95 : 0.7);
        p->drawPixmap(QPoint(title.left(), title.center().y() - side / 2), pm);
        p->setOpacity(1);
        title.setLeft(title.left() + side + 6);
        return title;
    }

    void paintClose(QPainter *p, const QRect &close) const
    {
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 160));
        p->drawEllipse(close);
        p->setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        const QRectF x = QRectF(close).adjusted(5.5, 5.5, -5.5, -5.5);
        p->drawLine(x.topLeft(), x.bottomRight());
        p->drawLine(x.topRight(), x.bottomLeft());
    }

    // A compact card's details, beside its thumbnail: the title (with the
    // new-output dot), then the badge as a chip, and the program's status,
    // if it has one, ahead of the subtitle.
    void paintCompactDetails(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index,
                             const QRect &preview, const QColor &fill, const QColor &accent, bool selected,
                             bool hovered) const
    {
        const QRect card = cardRect(option.rect);
        const QFont small = smallFont(option.font);
        QFont titleFont = option.font;
        titleFont.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        const QFontMetrics tm(titleFont), sm(small);
        int right = card.right() - kPad;
        if (hovered) {
            const QRect close = closeRect(option.rect);
            paintClose(p, close);
            right = close.left() - 6;
        }
        const int left = preview.right() + 9;
        if (right - left < 8)
            return;
        p->save();
        p->setClipRect(QRect(left, card.top(), right - left + 1, card.height()));

        const int block = tm.height() + 2 + sm.height();
        QRect title(left, card.center().y() - block / 2 + 1, right - left + 1, tm.height());
        if (index.data(kBusyRole).toBool()) {
            cardstyle::paintDot(p, QPointF(title.right() - kDot / 2.0, title.center().y() + 0.5), accent);
            title.setRight(title.right() - kDot - 6);
        }
        title = paintIcon(p, index, title, tm.height(), selected);
        p->setFont(titleFont);
        p->setPen(foreground);
        p->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                    tm.elidedText(index.data(kTitleRole).toString(), Qt::ElideRight, title.width()));

        QRect sub(left, title.bottom() + 3, right - left + 1, sm.height());
        p->setFont(small);
        const QString badge = index.data(kBadgeRole).toString();
        if (!badge.isEmpty()) {
            const QString text = sm.elidedText(badge, Qt::ElideRight, sub.width() / 2);
            const QRect chip(sub.left(), sub.top(), sm.horizontalAdvance(text) + 10, sm.height());
            p->setPen(Qt::NoPen);
            p->setBrush(mix(fill, foreground, 0.18));
            p->drawRoundedRect(chip, chip.height() / 2.0, chip.height() / 2.0);
            p->setPen(foreground);
            p->drawText(chip, Qt::AlignCenter, text);
            sub.setLeft(chip.right() + 5);
        }
        QString rest = index.data(kSubtitleRole).toString();
        const auto kind = cardstyle::Status::Kind(index.data(kStatusKindRole).toInt());
        if (kind != cardstyle::Status::None) {
            const QColor colour = cardstyle::statusColour(kind);
            p->setPen(Qt::NoPen);
            p->setBrush(colour);
            p->drawEllipse(QPointF(sub.left() + 3, sub.center().y() + 0.5), 3, 3);
            sub.setLeft(sub.left() + 10);
            const QString label = sm.elidedText(index.data(kStatusLabelRole).toString(), Qt::ElideRight, sub.width());
            p->setPen(colour);
            p->drawText(sub, Qt::AlignLeft | Qt::AlignVCenter, label);
            sub.setLeft(sub.left() + sm.horizontalAdvance(label));
            if (!rest.isEmpty())
                rest = QStringLiteral(" · ") + rest;
        }
        if (!rest.isEmpty() && sub.width() > sm.horizontalAdvance(QStringLiteral(" · …")) + 6) {
            p->setPen(mix(foreground, fill, 0.45));
            p->drawText(sub, Qt::AlignLeft | Qt::AlignVCenter, sm.elidedText(rest, Qt::ElideMiddle, sub.width()));
        }
        p->restore();
    }

    // A group's header: a chevron, its icon and name, and how many cards.
    void paintHeader(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool collapsed = index.data(kCollapsedRole).toBool();
        const QRect row = option.rect.adjusted(kSide, 0, -kSide, 0);
        const QColor text = mix(background, foreground, hovered ? 0.95 : 0.7);
        const QFont small = smallFont(option.font);
        QFont font = small;
        font.setWeight(QFont::DemiBold);
        const QFontMetrics fm(font);
        const int mid = row.top() + (row.height() + 2) / 2;

        // The chevron: right when collapsed, down when open.
        QPainterPath chevron;
        const QPointF c(row.left() + 6, mid);
        if (collapsed) {
            chevron.moveTo(c + QPointF(-1.5, -3.5));
            chevron.lineTo(c + QPointF(2, 0));
            chevron.lineTo(c + QPointF(-1.5, 3.5));
        } else {
            chevron.moveTo(c + QPointF(-3.5, -1.5));
            chevron.lineTo(c + QPointF(0, 2));
            chevron.lineTo(c + QPointF(3.5, -1.5));
        }
        p->setPen(QPen(text, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawPath(chevron);

        int x = row.left() + 16;
        const QString icon = index.data(kGroupIconRole).toString();
        if (!icon.isEmpty()) {
            const int side = fm.height() - 2;
            const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
            p->setOpacity(hovered ? 0.95 : 0.75);
            p->drawPixmap(QPoint(x, mid - side / 2), appIcon(icon.toLatin1().constData(), foreground)
                                                         .pixmap(QSize(side, side), dpr));
            p->setOpacity(1);
            x += side + 6;
        }
        const QString count = QString::number(index.data(kCountRole).toInt());
        const int countWidth = QFontMetrics(small).horizontalAdvance(count) + 12;
        int right = row.right() - countWidth - 4;

        // Shut, it says what its hidden cards would: how many programs are
        // waiting on the user, and a dot for output not yet seen.
        if (collapsed) {
            if (const int blocked = index.data(kBlockedCountRole).toInt(); blocked > 0) {
                const QColor colour = cardstyle::statusColour(cardstyle::Status::Blocked);
                const QString text = blocked == 1 ? QObject::tr("needs you") : QObject::tr("%1 need you").arg(blocked);
                const QFontMetrics sm(small);
                const int w = sm.horizontalAdvance(text) + 18;
                const QRect pill(right - w, mid - sm.height() / 2, w, sm.height());
                p->setPen(Qt::NoPen);
                p->setBrush(mix(background, colour, 0.22));
                p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
                p->setBrush(colour);
                p->drawEllipse(QPointF(pill.left() + 7, pill.center().y() + 0.5), 2.5, 2.5);
                p->setFont(small);
                p->setPen(colour);
                p->drawText(pill.adjusted(12, 0, 0, 0), Qt::AlignCenter, text);
                right = pill.left() - 6;
            }
            if (index.data(kAnyBusyRole).toBool()) {
                cardstyle::paintDot(p, QPointF(right - kDot / 2.0, mid + 0.5), cardstyle::accent());
                right -= kDot + 6;
            }
        }
        const QRect label(x, row.top(), right - x, row.height());
        p->setFont(font);
        p->setPen(text);
        p->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(index.data(kGroupLabelRole).toString(), Qt::ElideRight, label.width()));
        const QRect pill(row.right() - countWidth, mid - fm.height() / 2, countWidth, fm.height());
        p->setPen(Qt::NoPen);
        p->setBrush(mix(background, foreground, 0.12));
        p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        p->setFont(small);
        p->setPen(text);
        p->drawText(pill, Qt::AlignCenter, count);
        p->restore();
    }

    int headerHeight(const QFont &font) const
    {
        return QFontMetrics(smallFont(font)).height() + 14;
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.data(kHeaderRole).toBool()) {
            paintHeader(p, option, index);
            return;
        }
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QRect card = cardRect(option.rect);

        // Read each time, so a change of system accent shows at once.
        const QColor accent = cardstyle::accent();
        const auto kind = cardstyle::Status::Kind(index.data(kStatusKindRole).toInt());
        const QColor fill = cardstyle::paintCard(
            p, card, background, foreground, accent, hovered, selected,
            tintBlocked && kind == cardstyle::Status::Blocked ? cardstyle::statusColour(kind) : QColor());

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
        const int progress = index.data(kStatusProgressRole).toInt();
        if (compact && kind != cardstyle::Status::None && progress >= 0) {
            // Along the thumbnail's bottom edge, inside its corners.
            const QRectF track(preview.left(), preview.bottom() - 2, preview.width(), 3);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 120));
            p->drawRect(track);
            p->setBrush(cardstyle::statusColour(kind));
            p->drawRect(QRectF(track.topLeft(), QSizeF(track.width() * progress / 100.0, track.height())));
        }
        p->restore();
        p->setPen(QPen(mix(fill, foreground, 0.15), 1));
        p->setBrush(Qt::NoBrush);
        p->drawPath(previewPath);

        if (compact) {
            paintCompactDetails(p, option, index, preview, fill, accent, selected, hovered);
            p->restore();
            return;
        }

        // A badge on the preview: where the tab is (its tmux or Rex
        // session, or the host).
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

        // What the program says it's doing, top left; its progress along
        // the bottom.
        if (kind != cardstyle::Status::None) {
            cardstyle::Status status;
            status.kind = kind;
            status.label = index.data(kStatusLabelRole).toString();
            cardstyle::paintStatusPill(p, preview.topLeft() + QPoint(4, 4), status, small,
                                       preview.width() - 2 * kClose);
            if (progress >= 0) {
                const QRectF track(preview.left() + 1, preview.bottom() - 3, preview.width() - 2, 3);
                p->setPen(Qt::NoPen);
                p->setBrush(QColor(0, 0, 0, 120));
                p->drawRect(track);
                p->setBrush(cardstyle::statusColour(kind));
                p->drawRect(QRectF(track.topLeft(), QSizeF(track.width() * progress / 100.0, track.height())));
            }
        }

        // Close button, only while hovered.
        if (hovered)
            paintClose(p, closeRect(option.rect));

        // Title, with the new-output dot at its end.
        const bool busy = index.data(kBusyRole).toBool();
        const QFontMetrics titleMetrics(option.font);
        QRect title(preview.left(), preview.bottom() + 1 + 6, preview.width(),
                    titleMetrics.height());
        if (busy) {
            cardstyle::paintDot(p, QPointF(title.right() - kDot / 2.0, title.center().y() + 0.5),
                                accent);
            title.setRight(title.right() - kDot - 6);
        }
        QFont titleFont = option.font;
        titleFont.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        title = paintIcon(p, index, title, titleMetrics.height(), selected);
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
    setMinimumWidth(kMinWidth);
    setMaximumWidth(520);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setAutoFillBackground(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, kGap, 0, kGap);
    outer->setSpacing(0);

    m_workspace = new FlatToolButton(this);
    m_workspace->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_workspace->setPopupMode(QToolButton::InstantPopup);
    m_workspace->setAutoRaise(true);
    m_workspace->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_workspace->setToolTip(tr("Switch workspace"));
    QFont headerFont = m_workspace->font();
    headerFont.setWeight(QFont::DemiBold);
    m_workspace->setFont(headerFont);
    m_workspace->hide();
    auto *header = new QHBoxLayout;
    header->setContentsMargins(kSide, 0, kSide, kGap);
    header->addWidget(m_workspace);
    outer->addLayout(header);

    m_list = new QListWidget(this);
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setUniformItemSizes(false);   // group headers are shorter
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

    m_newTab = new FlatToolButton(this);
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
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        const int tab = tabOf(item);
        if (tab >= 0 && !m_syncing)
            emit tabActivated(tab);
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        emit contextMenuRequested(tabOf(m_list->itemAt(pos)), m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int start, int, const QModelIndex &, int row) {
                if (m_syncing)
                    return;
                const int at = row > start ? row - 1 : row;
                QListWidgetItem *moved = m_list->item(at);
                const int from = tabOf(moved);
                if (from < 0)
                    return;
                int to = -1;
                if (!m_grouped || m_headers.isEmpty()) {
                    // Rows are tabs, in order.
                    to = at;
                } else {
                    // A card stays in its group: it goes after the card of
                    // its group above it, or before the one below.
                    const QString group = moved->data(kGroupRole).toString();
                    const auto sameGroup = [&](int r) {
                        QListWidgetItem *it = m_list->item(r);
                        return it && !it->data(kHeaderRole).toBool() && it->data(kGroupRole).toString() == group
                                   ? tabOf(it) : -1;
                    };
                    if (const int above = sameGroup(at - 1); above >= 0)
                        to = above < from ? above + 1 : above;
                    else if (const int below = sameGroup(at + 1); below >= 0)
                        to = below < from ? below : below - 1;
                    schedulePlace();   // dropped elsewhere: back where it belongs
                }
                if (to < 0 || to == from)
                    return;
                m_cards.move(from, to);
                renumber();
                emit tabMoved(from, to);
            });

    setColours(palette().color(QPalette::Base), palette().color(QPalette::Text));
}

QListWidgetItem *ThumbnailSidebar::card(int index) const
{
    return (index >= 0 && index < m_cards.size()) ? m_cards.at(index) : nullptr;
}

int ThumbnailSidebar::tabOf(const QListWidgetItem *item) const
{
    return item && !item->data(kHeaderRole).toBool() ? item->data(kTabRole).toInt() : -1;
}

void ThumbnailSidebar::renumber()
{
    for (int i = 0; i < m_cards.size(); ++i)
        m_cards.at(i)->setData(kTabRole, i);
}

void ThumbnailSidebar::syncTabs(const QStringList &titles, int currentIndex)
{
    m_syncing = true;
    while (m_cards.size() > titles.size()) {
        QListWidgetItem *item = m_cards.takeLast();
        delete m_list->takeItem(m_list->row(item));
    }
    for (int i = 0; i < titles.size(); ++i) {
        if (i == m_cards.size()) {
            auto *item = new QListWidgetItem(m_list);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            m_cards << item;
        }
        setTitle(i, titles.at(i));
    }
    renumber();
    placeItems();
    updateItemSizes();
    if (QListWidgetItem *item = card(currentIndex))
        m_list->setCurrentItem(item);
    m_syncing = false;
}

void ThumbnailSidebar::setCurrentIndex(int index)
{
    QListWidgetItem *item = card(index);
    if (!item)
        return;
    // A tab chosen another way shows its card, if its group was shut.
    if (item->isHidden())
        toggleGroup(item->data(kGroupRole).toString());
    m_syncing = true;
    m_list->setCurrentItem(item);
    m_syncing = false;
}

void ThumbnailSidebar::setTitle(int index, const QString &title)
{
    if (QListWidgetItem *item = card(index)) {
        const QString text = title.isEmpty() ? tr("Terminal") : title;
        item->setData(kTitleRole, text);
        updateToolTip(item);
    }
}

void ThumbnailSidebar::updateToolTip(QListWidgetItem *item)
{
    // The title, and under it what a program there says it's doing. Both
    // come from programs, so escaped: Qt would take text that looks like
    // HTML as HTML.
    const QString detail = item->data(kStatusDetailRole).toString();
    item->setToolTip(QStringLiteral("<qt>") + item->data(kTitleRole).toString().toHtmlEscaped()
                     + (detail.isEmpty() ? QString() : QStringLiteral("<br>") + detail.toHtmlEscaped()));
}

void ThumbnailSidebar::setSubtitle(int index, const QString &subtitle)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kSubtitleRole, subtitle);
}

void ThumbnailSidebar::setIcon(int index, const QString &icon)
{
    if (QListWidgetItem *item = card(index))
        item->setData(kIconRole, icon);
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

void ThumbnailSidebar::setStatus(int index, const cardstyle::Status &status)
{
    QListWidgetItem *item = card(index);
    if (!item)
        return;
    item->setData(kStatusKindRole, int(status.kind));
    item->setData(kStatusLabelRole, status.label);
    item->setData(kStatusProgressRole, status.kind == cardstyle::Status::None ? -1 : status.progress);
    item->setData(kStatusDetailRole, status.detail);
    updateToolTip(item);
    updateHeader(item->data(kGroupRole).toString());
}

void ThumbnailSidebar::setTintBlocked(bool tint)
{
    auto *delegate = delegateOf(m_list);
    if (delegate->tintBlocked == tint)
        return;
    delegate->tintBlocked = tint;
    m_list->viewport()->update();
}

void ThumbnailSidebar::setBusy(int index, bool busy)
{
    if (QListWidgetItem *item = card(index)) {
        item->setData(kBusyRole, busy);
        updateHeader(item->data(kGroupRole).toString());
    }
}

void ThumbnailSidebar::moveCard(int from, int to)
{
    if (!card(from) || !card(to) || from == to)
        return;
    m_cards.move(from, to);
    renumber();
    placeItems();
}

void ThumbnailSidebar::setGrouped(bool grouped)
{
    if (grouped == m_grouped)
        return;
    m_grouped = grouped;
    placeItems();
}

void ThumbnailSidebar::setGroup(int index, const QString &key, const QString &label, const QString &icon)
{
    QListWidgetItem *item = card(index);
    if (!item)
        return;
    const bool moved = item->data(kGroupRole).toString() != key;
    item->setData(kGroupRole, key);
    item->setData(kGroupLabelRole, label);
    item->setData(kGroupIconRole, icon);
    if (QListWidgetItem *header = m_headers.value(key)) {
        header->setData(kGroupLabelRole, label);
        header->setData(kGroupIconRole, icon);
    }
    if (moved && m_grouped)
        schedulePlace();
}

void ThumbnailSidebar::schedulePlace()
{
    if (m_placePending)
        return;
    m_placePending = true;
    QTimer::singleShot(0, this, [this]() {
        m_placePending = false;
        placeItems();
    });
}

void ThumbnailSidebar::toggleGroup(const QString &key)
{
    if (!m_collapsed.remove(key))
        m_collapsed.insert(key);
    placeItems();
}

void ThumbnailSidebar::updateHeader(const QString &key)
{
    QListWidgetItem *header = m_headers.value(key);
    if (!header)
        return;
    int blocked = 0;
    bool busy = false;
    for (QListWidgetItem *c : std::as_const(m_cards)) {
        if (c->data(kGroupRole).toString() != key)
            continue;
        blocked += c->data(kStatusKindRole).toInt() == int(cardstyle::Status::Blocked);
        busy = busy || c->data(kBusyRole).toBool();
    }
    header->setData(kBlockedCountRole, blocked);
    header->setData(kAnyBusyRole, busy);
    const bool collapsed = m_collapsed.contains(key);
    QString tip = collapsed ? tr("Show this group's tabs") : tr("Hide this group's tabs");
    if (collapsed && blocked > 0)
        tip += QStringLiteral("\n") + tr("%n program(s) waiting on you", nullptr, blocked);
    header->setToolTip(tip);
}

void ThumbnailSidebar::placeItems()
{
    // The order wanted: the cards in tab order, or under their headers.
    QList<QListWidgetItem *> order;
    QHash<QString, QListWidgetItem *> headers;
    QSet<QListWidgetItem *> hidden;
    QStringList groups;
    QHash<QString, QList<QListWidgetItem *>> members;
    for (QListWidgetItem *item : std::as_const(m_cards)) {
        const QString key = item->data(kGroupRole).toString();
        if (!members.contains(key))
            groups << key;
        members[key] << item;
    }
    if (!m_grouped || groups.size() < 2) {
        order = m_cards;
    } else {
        for (const QString &key : std::as_const(groups)) {
            QListWidgetItem *header = m_headers.take(key);
            if (!header) {
                header = new QListWidgetItem;
                header->setFlags(Qt::ItemIsEnabled);
                header->setData(kHeaderRole, true);
                header->setData(kGroupRole, key);
            }
            const QList<QListWidgetItem *> &cards = members.value(key);
            header->setData(kGroupLabelRole, cards.first()->data(kGroupLabelRole));
            header->setData(kGroupIconRole, cards.first()->data(kGroupIconRole));
            header->setData(kCountRole, int(cards.size()));
            const bool collapsed = m_collapsed.contains(key);
            header->setData(kCollapsedRole, collapsed);
            headers.insert(key, header);
            order << header;
            for (QListWidgetItem *c : cards) {
                order << c;
                if (collapsed)
                    hidden.insert(c);
            }
        }
    }
    // Headers of groups that have gone.
    for (QListWidgetItem *gone : std::as_const(m_headers)) {
        if (const int row = m_list->row(gone); row >= 0)
            m_list->takeItem(row);
        delete gone;
    }
    m_headers = headers;
    for (auto it = m_headers.cbegin(); it != m_headers.cend(); ++it)
        updateHeader(it.key());
    // Forget collapsed groups that are no more.
    for (auto it = m_collapsed.begin(); it != m_collapsed.end();)
        it = headers.contains(*it) ? std::next(it) : m_collapsed.erase(it);

    bool same = m_list->count() == order.size();
    for (int i = 0; same && i < order.size(); ++i)
        same = m_list->item(i) == order.at(i);
    const bool wasSyncing = m_syncing;
    m_syncing = true;
    if (!same) {
        QListWidgetItem *current = m_list->currentItem();
        const int scroll = m_list->verticalScrollBar()->value();
        while (m_list->count() > 0)
            m_list->takeItem(0);
        for (QListWidgetItem *item : std::as_const(order))
            m_list->addItem(item);
        if (current)
            m_list->setCurrentItem(current);
        m_list->verticalScrollBar()->setValue(scroll);
    }
    for (QListWidgetItem *item : std::as_const(m_cards))
        item->setHidden(hidden.contains(item));
    m_syncing = wasSyncing;
    if (!same)
        updateItemSizes();
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

void ThumbnailSidebar::setCompact(bool compact)
{
    if (compact == m_compact)
        return;
    m_compact = compact;
    delegateOf(m_list)->compact = compact;
    // Compact cards need room beside the thumbnail for their details.
    setMinimumWidth(compact ? kCompactMinWidth : kMinWidth);
    updateItemSizes();
    m_list->viewport()->update();
}

QSize ThumbnailSidebar::previewSize() const
{
    // A compact thumbnail is drawn at its natural size whatever the width
    // (a narrow sidebar crops it), so resizing doesn't redraw previews.
    if (m_compact)
        return thumbSize(m_aspect);
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

    const QColor side = cardstyle::surface(background, foreground);
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
    m_workspace->setPalette(pal);

    m_newTab->setIcon(cardstyle::plusIcon(foreground, devicePixelRatioF()));

    auto *delegate = delegateOf(m_list);
    delegate->background = side;
    delegate->foreground = foreground;

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
    const int header = delegateOf(m_list)->headerHeight(m_list->font());
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        item->setSizeHint(QSize(width, item->data(kHeaderRole).toBool() ? header : height));
    }
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
        if (index.isValid() && index.data(kHeaderRole).toBool()) {
            if (me->button() == Qt::LeftButton)
                toggleGroup(index.data(kGroupRole).toString());
            return true;
        }
        if (index.isValid()) {
            // Middle click closes, as on a tab bar; so does the hover ✕.
            const bool onClose = me->button() == Qt::LeftButton
                && delegateOf(m_list)->closeRect(m_list->visualRect(index))
                       .contains(me->position().toPoint());
            if (me->button() == Qt::MiddleButton || onClose) {
                emit tabClosed(index.data(kTabRole).toInt());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ThumbnailSidebar::setWorkspace(const QString &name, const QIcon &icon, QMenu *menu, bool show)
{
    m_workspace->setText(name);
    m_workspace->setIcon(icon);
    if (m_workspace->menu() != menu)
        m_workspace->setMenu(menu);
    m_workspace->setVisible(show);
}
