/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRect>

/*
 * How a tab is drawn, shared by the thumbnail sidebar's cards and the tab
 * strip along the top, so the two layouts look like one design. Both draw in
 * the terminal scheme's colours rather than the window's, so the tabs read as
 * part of the terminal.
 */
namespace cardstyle {

constexpr int kRadius = 6;   // card corners
constexpr int kDot = 8;      // the new-output dot

inline QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

/*
 * The current tab's tint and outline, and the new-output dot: the system's
 * accent colour. On macOS Highlight is the muted text-selection colour, not
 * the accent; elsewhere the two agree.
 */
inline QColor accent()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    return QGuiApplication::palette().color(QPalette::Accent);
#else
    return QGuiApplication::palette().color(QPalette::Highlight);
#endif
}

/*
 * What the cards sit on: lifted a little off the terminal's background, so
 * the two read as one surface with a seam rather than as unrelated widgets.
 */
inline QColor surface(const QColor &background, const QColor &foreground)
{
    return mix(background, foreground, 0.04);
}

/*
 * A card: lifted off the surface, more so under the mouse; the current tab is
 * tinted and outlined in the accent colour. Returns its fill, for colours
 * drawn on top of it.
 */
inline QColor paintCard(QPainter *p, const QRect &card, const QColor &surface,
                        const QColor &foreground, const QColor &accent,
                        bool hovered, bool selected)
{
    QColor fill = mix(surface, foreground, hovered ? 0.12 : 0.07);
    if (selected)
        fill = mix(fill, accent, 0.22);
    QPainterPath path;
    path.addRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    p->fillPath(path, fill);
    if (selected) {
        p->setPen(QPen(accent, 2));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(card).adjusted(1, 1, -1, -1), kRadius - 1, kRadius - 1);
    }
    return fill;
}

/* The mark for output you haven't seen, centred on `centre`. */
inline void paintDot(QPainter *p, const QPointF &centre, const QColor &accent)
{
    p->setPen(Qt::NoPen);
    p->setBrush(accent);
    p->drawEllipse(centre, kDot / 2.0, kDot / 2.0);
}

/*
 * A plain plus in the given colour, for New Tab on a surface in the terminal's
 * colours: icons follow the window's palette, and a dark one vanishes on a
 * dark scheme under a light window (or the reverse).
 */
inline QIcon plusIcon(const QColor &colour, qreal dpr)
{
    QPixmap plus(QSize(16, 16) * dpr);
    plus.setDevicePixelRatio(dpr);
    plus.fill(Qt::transparent);
    QPainter p(&plus);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(8, 3), QPointF(8, 13));
    p.drawLine(QPointF(3, 8), QPointF(13, 8));
    p.end();
    return QIcon(plus);
}

/* A down chevron in the given colour, for the strip's list of all tabs. */
inline QIcon chevronIcon(const QColor &colour, qreal dpr)
{
    QPixmap chevron(QSize(16, 16) * dpr);
    chevron.setDevicePixelRatio(dpr);
    chevron.fill(Qt::transparent);
    QPainter p(&chevron);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(colour, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF points[] = {{4, 6}, {8, 10}, {12, 6}};
    p.drawPolyline(points, 3);
    p.end();
    return QIcon(chevron);
}

/* The new-output dot as a menu icon, centred in its square. */
inline QIcon dotIcon(qreal dpr)
{
    QPixmap dot(QSize(16, 16) * dpr);
    dot.setDevicePixelRatio(dpr);
    dot.fill(Qt::transparent);
    QPainter p(&dot);
    p.setRenderHint(QPainter::Antialiasing);
    paintDot(&p, QPointF(8, 8), accent());
    p.end();
    return QIcon(dot);
}

} // namespace cardstyle
