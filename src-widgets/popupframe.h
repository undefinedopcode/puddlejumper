/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "cardstyle.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRect>

/*
 * The frame the app's own popups share (the command palette, a Rex server's
 * prompts, About): a frameless, translucent window holding a rounded panel
 * with a soft shadow, kShadow logical pixels of it on every side. The window
 * sets Qt::FramelessWindowHint and WA_TranslucentBackground, keeps its
 * content inside the shadow, and calls paint() from its paintEvent().
 */
namespace popupframe {

constexpr int kShadow = 14;  // room around the panel for its shadow
constexpr int kRadius = 12;  // the panel's corners

// Quieter text: headings, notes, hints.
inline QColor muted(const QPalette &pal)
{
    return cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.58);
}

// The panel's colour: the window's, a touch towards the text, so it stands
// off whatever it is over.
inline QColor panelColour(const QPalette &pal)
{
    return cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.03);
}

// The shadow, panel and border, for a window of `rect`.
inline void paint(QPainter &p, const QRect &rect, const QPalette &pal)
{
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF panel = QRectF(rect).adjusted(kShadow, kShadow, -kShadow, -kShadow);
    // A soft shadow: rings fading out from the panel.
    for (int i = kShadow; i > 0; --i) {
        QPainterPath ring;
        ring.addRoundedRect(panel.adjusted(-i, -i + 4, i, i + 4).intersected(QRectF(rect)), kRadius + i, kRadius + i);
        const qreal t = 1.0 - qreal(i) / kShadow;
        p.fillPath(ring, QColor(0, 0, 0, int(6 * t * t)));
    }
    QPainterPath path;
    path.addRoundedRect(panel, kRadius, kRadius);
    p.fillPath(path, panelColour(pal));
    p.setPen(QPen(cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.16), 1));
    p.drawPath(path);
}

} // namespace popupframe
