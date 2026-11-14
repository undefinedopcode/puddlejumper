/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QWidget>

class VncClient;

/*
 * A remote screen over VNC: draws the framebuffer scaled to fit (never
 * enlarged), and sends the mouse and keyboard to it.
 *
 * While it has the focus every key goes to the remote end, application
 * shortcuts included; keys go as scan codes where the server takes them, so
 * the remote keyboard layout applies. Keys still held when the focus leaves
 * are released, so none stay stuck down on the remote end.
 */
class VncView : public QWidget
{
    Q_OBJECT

public:
    explicit VncView(QWidget *parent = nullptr);

    void connectToHost(const QString &host, quint16 port);
    VncClient *client() const { return m_client; }
    /* The remote desktop's name, or where it is while connecting. */
    QString title() const;

    QSize sizeHint() const override;

    /*
     * "host", "host:port", or "host:display" as VNC viewers take it (a
     * number under 100 is a display, port 5900 + display). False if it
     * isn't one of those.
     */
    static bool parseAddress(const QString &text, QString *host, quint16 *port);
    /* A window of its own, deleted on close. */
    static VncView *openWindow(const QString &host, quint16 port);

signals:
    void titleChanged(const QString &title);

protected:
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    struct HeldKey
    {
        quint32 keysym;
        quint32 scanCode;
    };

    /* Where the framebuffer is drawn, in widget coordinates. */
    QRectF imageRect() const;
    QPoint toRemote(const QPointF &pos) const;
    void sendPointer(const QPointF &pos, Qt::MouseButtons buttons);
    void sendKey(QKeyEvent *event, bool down);
    void releaseKeys();
    void setStatus(const QString &status);
    void updateCursor();

    VncClient *m_client = nullptr;
    QString m_address;            // host:port
    QString m_status;             // shown instead of the screen; empty once up
    QHash<quint32, HeldKey> m_held;   // by the key's identity (see sendKey())
    QPoint m_wheel;               // angle delta not yet sent as wheel clicks
    int m_buttons = 0;            // VNC button mask last sent
    QPoint m_lastPos;
    QString m_sentClipboard;      // last clipboard text exchanged either way
    QImage m_cursor;
    QPoint m_cursorHotspot;
};
