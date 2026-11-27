/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "flattoolbutton.h"

#include "cardstyle.h"

#include <QStyleOptionToolButton>
#include <QStylePainter>

namespace {
constexpr int kPad = 6;       // inside the fill, beside the label
constexpr int kChevron = 12;  // the drop-down's chevron
constexpr qreal kRadius = 6;
}

QSize FlatToolButton::sizeHint() const
{
    const QSize base = QToolButton::sizeHint();
    if (toolButtonStyle() == Qt::ToolButtonIconOnly)
        return base;
    int w = 2 * kPad;
    if (!icon().isNull())
        w += iconSize().width() + 4;   // the style puts 4 between icon and text
    w += fontMetrics().horizontalAdvance(text());
    if (popupMode() == QToolButton::InstantPopup && menu())
        w += kChevron + 2;
    return {w, base.height()};
}

void FlatToolButton::paintEvent(QPaintEvent *)
{
    QStylePainter p(this);
    QStyleOptionToolButton opt;
    initStyleOption(&opt);

    const bool dropDown = popupMode() == QToolButton::InstantPopup && (opt.features & QStyleOptionToolButton::HasMenu);
    const bool down = opt.state & (QStyle::State_Sunken | QStyle::State_On);
    const bool hover = isEnabled() && (opt.state & QStyle::State_MouseOver);
    const QColor text = palette().color(QPalette::ButtonText);
    if (down || hover) {
        QColor fill = palette().color(QPalette::Button);
        if (down)
            fill = cardstyle::mix(fill, text, 0.08);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()), kRadius, kRadius);
    }

    // The label alone: no bevel, no native arrow, no shift when pressed.
    QStyleOptionToolButton label = opt;
    label.state &= ~(QStyle::State_Sunken | QStyle::State_On | QStyle::State_MouseOver | QStyle::State_Raised);
    label.features &= ~(QStyleOptionToolButton::HasMenu | QStyleOptionToolButton::MenuButtonPopup);
    label.palette.setColor(QPalette::ButtonText, text);
    if (toolButtonStyle() != Qt::ToolButtonIconOnly)
        label.rect = rect().adjusted(kPad, 0, -kPad - (dropDown ? kChevron + 2 : 0), 0);
    p.drawControl(QStyle::CE_ToolButtonLabel, label);

    if (dropDown) {
        const QPixmap chevron = cardstyle::chevronIcon(text, devicePixelRatioF()).pixmap(QSize(kChevron, kChevron), devicePixelRatioF());
        p.drawPixmap(QPoint(width() - kPad - kChevron, (height() - kChevron) / 2), chevron);
    }
}
