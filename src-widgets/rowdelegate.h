#pragma once

#include "cardstyle.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>

/*
 * List rows as the prompts and the palette draw them: the chosen row a
 * rounded accent tint (a fainter one under the mouse), its icon and text in
 * the ordinary text colour rather than the style's highlight block. For the
 * settings' and other dialogs' lists.
 */
class RowDelegate : public QStyledItemDelegate
{
public:
    explicit RowDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QPalette &pal = opt.palette;
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const bool enabled = opt.state & QStyle::State_Enabled;

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(opt.rect).adjusted(4, 1, -4, -1);
        if (selected || hovered) {
            QPainterPath path;
            path.addRoundedRect(row, 7, 7);
            p->fillPath(path, cardstyle::mix(pal.color(QPalette::Base), pal.color(QPalette::Highlight),
                                             selected ? 0.22 : 0.08));
        }
        int x = int(row.left()) + 8;
        if (!opt.icon.isNull()) {
            const QSize size = opt.decorationSize;
            const QRect r(x, opt.rect.center().y() - size.height() / 2 + 1, size.width(), size.height());
            opt.icon.paint(p, r, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
            x += size.width() + 8;
        }
        QColor text = pal.color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Text);
        if (const QVariant fg = index.data(Qt::ForegroundRole); fg.canConvert<QBrush>())
            text = fg.value<QBrush>().color();
        QFont font = opt.font;
        if (selected)
            font.setWeight(QFont::DemiBold);
        p->setFont(font);
        p->setPen(text);
        const QRect textRect(x, opt.rect.top(), int(row.right()) - 8 - x, opt.rect.height());
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(opt.text, Qt::ElideRight, textRect.width()));
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), option.fontMetrics.height() + 14));
        return size;
    }

    /* Rows in this style for `view`, with hover tracked. */
    template <typename View>
    static void install(View *view)
    {
        view->setItemDelegate(new RowDelegate(view));
        view->setMouseTracking(true);
        view->viewport()->setAttribute(Qt::WA_Hover);
    }
};
