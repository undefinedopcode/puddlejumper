/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "screenshotoverlay.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>

#include <cmath>

ScreenshotOverlay::ScreenshotOverlay(QWidget *display, const QRect &cells,
                                     const QSize &cellSize)
    : QWidget(display)
    , m_cells(cells)
    , m_cellSize(cellSize)
{
    // Grabbed before show(), so the overlay itself isn't in the image.
    m_snapshot = display->grab();

    setGeometry(display->rect());
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    display->installEventFilter(this);
}

QPixmap ScreenshotOverlay::crop(const QPixmap &grab, const QRect &rect)
{
    const qreal dpr = grab.devicePixelRatio();
    const QRect device = QRectF(QPointF(rect.topLeft()) * dpr, QSizeF(rect.size()) * dpr)
                             .toAlignedRect();
    QPixmap out = grab.copy(device);
    out.setDevicePixelRatio(dpr);
    return out;
}

QRect ScreenshotOverlay::selection() const
{
    const QRect raw = QRect(m_anchor, m_pos).normalized();
    if (m_free)
        return raw.intersected(rect());

    // Every cell the drag touches is included, so a click-and-release on a
    // single character captures that one cell.
    const int cw = m_cellSize.width();
    const int ch = m_cellSize.height();
    const int cols = m_cells.width() / cw;
    const int rows = m_cells.height() / ch;
    if (cols < 1 || rows < 1)
        return {};

    const auto col = [&](int x) {
        return qBound(0, int(std::floor(double(x - m_cells.left()) / cw)), cols - 1);
    };
    const auto row = [&](int y) {
        return qBound(0, int(std::floor(double(y - m_cells.top()) / ch)), rows - 1);
    };
    const int c0 = col(raw.left());
    const int c1 = col(raw.right());
    const int r0 = row(raw.top());
    const int r1 = row(raw.bottom());
    return {m_cells.left() + c0 * cw, m_cells.top() + r0 * ch,
            (c1 - c0 + 1) * cw, (r1 - r0 + 1) * ch};
}

void ScreenshotOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.drawPixmap(0, 0, m_snapshot);

    const QRect sel = m_dragging ? selection() : QRect();

    // Dim everything outside the selection.
    p.save();
    p.setClipRegion(QRegion(rect()).subtracted(sel));
    p.fillRect(rect(), QColor(0, 0, 0, 120));
    p.restore();

    // A label in a rounded box below anchor (above it if there's no room):
    // the hint before dragging, the selection's size during.
    const auto drawLabel = [&](const QString &text, QPoint anchor) {
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        const QRect textRect = p.fontMetrics().boundingRect(text);
        QRect box(0, 0, textRect.width() + 16, textRect.height() + 8);
        box.moveCenter(anchor);
        box.moveTop(anchor.y() + 6);
        if (box.bottom() > height())
            box.moveBottom(qMin(height(), anchor.y() - 6));
        box.moveLeft(qBound(4, box.left(), qMax(4, width() - box.width() - 4)));

        QPainterPath path;
        path.addRoundedRect(QRectF(box), 4, 4);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(path, QColor(0, 0, 0, 200));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, text);
    };

    if (sel.isEmpty()) {
        drawLabel(tr("Drag to select  ·  Shift: free selection  ·  Esc: cancel"),
                  QPoint(width() / 2, 0));
        return;
    }

    QPen pen(palette().color(QPalette::Highlight));
    pen.setWidth(1);
    p.setPen(pen);
    p.drawRect(sel.adjusted(0, 0, -1, -1));

    const QString size = m_free
        ? tr("%1 × %2 px").arg(sel.width()).arg(sel.height())
        : tr("%1 × %2 cells").arg(sel.width() / m_cellSize.width())
                             .arg(sel.height() / m_cellSize.height());
    drawLabel(size, QPoint(sel.center().x(), sel.bottom()));
}

void ScreenshotOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        finish(false);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    m_anchor = m_pos = event->position().toPoint();
    m_free = event->modifiers() & Qt::ShiftModifier;
    m_dragging = true;
    update();
}

void ScreenshotOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_pos = event->position().toPoint();
    m_free = event->modifiers() & Qt::ShiftModifier;
    update();
}

void ScreenshotOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return;
    m_pos = event->position().toPoint();
    m_free = event->modifiers() & Qt::ShiftModifier;
    finish(!selection().isEmpty());
}

void ScreenshotOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        finish(false);
        return;
    }
    // Pressing or releasing Shift mid-drag switches mode without moving.
    if (event->key() == Qt::Key_Shift && m_dragging) {
        m_free = true;
        update();
    }
}

void ScreenshotOverlay::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Shift && m_dragging) {
        m_free = false;
        update();
    }
}

void ScreenshotOverlay::hideEvent(QHideEvent *)
{
    finish(false);
}

bool ScreenshotOverlay::eventFilter(QObject *watched, QEvent *event)
{
    // The frozen image and the cell grid no longer match a resized display.
    if (watched == parent() && event->type() == QEvent::Resize)
        finish(false);
    return QWidget::eventFilter(watched, event);
}

void ScreenshotOverlay::finish(bool accept)
{
    if (m_finished)
        return;
    m_finished = true;
    hide();
    if (accept)
        emit captured(crop(m_snapshot, selection()));
    else
        emit cancelled();
    deleteLater();
}
