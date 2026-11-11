/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

#include "settings.h"

class QTermWidget;
class QMenu;
class QPoint;

/*
 * One terminal tab.
 *
 * Bundles a QTermWidget with the bits the widgets API does not provide:
 * a stable id (for preview files), the live title, the ability to re-apply
 * the whole appearance in one call, and a right-click context menu that
 * reuses QTermWidget's built-in copy/paste, zoom and search actions.
 */
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    TerminalWidget(int id, const Settings &settings, QWidget *parent = nullptr);

    int id() const { return m_id; }

    QTermWidget *terminal() const { return m_terminal; }

    QString sessionTitle() const { return m_title; }

    /* Re-apply every appearance setting. Safe to call on live terminals. */
    void applyAppearance();

    /* Re-apply scrollback depth / scrollbar / flow-control. */
    void applyScrolling();

    /* Re-apply activity/silence monitoring. */
    void applyMonitoring();

    void sendText(const QString &text);
    void focusTerminal();

    /* A live preview of the terminal as painted. Empty if unavailable. */
    QPixmap previewPixmap(const QSize &size) const;

    /*
     * Content tracking.
     *
     * receivedData() fires from the pty, so it fires for hidden tabs too.
     * That makes it usable as a dirty flag: the sidebar only needs to
     * re-capture a tab whose output has changed since the last grab, instead
     * of rotating blindly through every tab.
     */
    bool hasNewOutput() const { return m_dirty; }
    void clearNewOutput() { m_dirty = false; }

    /* Monotonic count of output chunks, for cheap change detection. */
    quint64 outputSeq() const { return m_outputSeq; }

signals:
    void titleChanged(const QString &title);
    void sessionFinished();
    /* Coalesced so a burst of output does not spam the window. */
    void outputArrived();
    /* Ask the owner to adjust everyone's font size (the shared Settings). */
    void zoomInRequested();
    void zoomOutRequested();
    void zoomResetRequested();
    /*
     * Monitoring signals, fed from QTermWidget::activity() / silence() when
     * the corresponding scrolling/monitor* setting is on. The owner decides
     * how to surface them (tab marker, bell, &c).
     */
    void activityDetected();
    void silenceDetected();

private:
    void onOutput();
    void showContextMenu(const QPoint &globalPos);

    int m_id = 0;
    const Settings &m_settings;
    QTermWidget *m_terminal = nullptr;
    QString m_title;

    bool m_dirty = false;
    quint64 m_outputSeq = 0;
    quint64 m_reportedSeq = 0;

    bool m_canCopy = false;
};
