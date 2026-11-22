/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QPixmap>
#include <QRect>
#include <QWidget>

/*
 * Rectangular screenshot selection over a terminal display.
 *
 * Covers the display with a frozen image of it, taken when the overlay is
 * created, so output arriving mid-selection doesn't change what is captured.
 * The user drags out a rectangle; on release captured() fires with that part
 * of the image and the overlay deletes itself.
 *
 * Selections snap to whole character cells, so a capture never shows half a
 * glyph. Holding Shift selects freely to the pixel instead. (Not Alt: many
 * window managers take Alt+drag to move the window.)
 *
 * Escape, a right click, being hidden (switching tabs) or the display being
 * resized all cancel, firing cancelled() instead. Losing focus deliberately
 * doesn't: a selection started from a context menu would lose it to the
 * closing menu and cancel at once.
 */
class ScreenshotOverlay : public QWidget
{
    Q_OBJECT

public:
    /*
     * display: the widget to cover and capture.
     * cells:   the character grid, in display coordinates.
     * cellSize: one character cell.
     */
    ScreenshotOverlay(QWidget *display, const QRect &cells, const QSize &cellSize);

    /* Cut rect (logical pixels) out of a grab, honouring its device pixel ratio. */
    static QPixmap crop(const QPixmap &grab, const QRect &rect);

signals:
    void captured(const QPixmap &image);
    void cancelled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    /* The current selection, snapped to cells unless m_free. */
    QRect selection() const;
    /* Fire captured() or cancelled() once, then go away. */
    void finish(bool accept);

    QPixmap m_snapshot;
    QRect m_cells;
    QSize m_cellSize;

    QPoint m_anchor;
    QPoint m_pos;
    bool m_dragging = false;
    bool m_free = false;
    bool m_finished = false;
};
