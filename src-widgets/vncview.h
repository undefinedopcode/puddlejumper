/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QTimer;
class VncClient;

/*
 * A remote screen over VNC: draws the framebuffer scaled to fit (never
 * enlarged), and sends the mouse and keyboard to it.
 *
 * While it has the focus every key goes to the remote end but those
 * setShortcutFilter() keeps for the application; keys go as scan codes where
 * the server takes them, so the remote keyboard layout applies. Keys still
 * held when the focus leaves are released, so none stay stuck down on the
 * remote end. Once disconnected, a click or Return connects again.
 *
 * A server that wants a login gets it from a prompt over the view: a
 * password, or for a Mac's own login an account name too. The password is
 * kept (in memory only) for reconnecting, and asked for again if wrong.
 */
class VncView : public QWidget
{
    Q_OBJECT

public:
    explicit VncView(QWidget *parent = nullptr);

    void connectToHost(const QString &host, quint16 port);
    void reconnect();
    VncClient *client() const { return m_client; }
    /* host:port, as connected to. */
    QString address() const { return m_address; }
    /* 16-bit colour, for slow links (see VncClient); reconnects if connected. */
    void setReducedColour(bool on);

    /* The account name for a Mac's login, as last given (offered next time). */
    QString username() const { return m_username; }
    void setUsername(const QString &username) { m_username = username; }
    /* The remote desktop's name, or where it is while connecting. */
    QString title() const;

    /* Keys for which `filter` is true are left to the application's shortcuts. */
    void setShortcutFilter(std::function<bool(const QKeyEvent *)> filter)
    {
        m_shortcutFilter = std::move(filter);
    }

    QSize sizeHint() const override;

    /*
     * "host", "host:port", or "host:display" as VNC viewers take it (a
     * number under 100 is a display, port 5900 + display). False if it
     * isn't one of those.
     */
    static bool parseAddress(const QString &text, QString *host, quint16 *port);

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
    /* Prods the server into looking at its screen again (see the timer). */
    void startNudging();
    void nudge();
    void sendKey(QKeyEvent *event, bool down);
    void releaseKeys();
    void setStatus(const QString &status);
    void updateCursor();
    void showLoginPrompt(bool needsUsername, const QString &error);
    void submitLogin();
    void placeLoginPrompt();
    /* Into the prompt: its name field, unless the name is known. */
    void focusLoginPrompt();

    VncClient *m_client = nullptr;
    QString m_host;
    quint16 m_port = 0;
    QString m_address;            // host:port
    std::function<bool(const QKeyEvent *)> m_shortcutFilter;
    QString m_status;             // shown instead of the screen; empty once up
    bool m_connecting = false;
    QHash<quint32, HeldKey> m_held;   // by the key's identity (see sendKey())
    QPoint m_wheel;               // angle delta not yet sent as wheel clicks
    int m_buttons = 0;            // VNC button mask last sent
    QPoint m_lastPos;
    QTimer *m_nudgeTimer = nullptr;
    int m_nudgeStep = 0;
    bool m_nudge = true;
    QString m_sentClipboard;      // last clipboard text exchanged either way
    QImage m_cursor;
    QPoint m_cursorHotspot;
    QString m_username;
    QString m_password;           // null until given
    QString m_loginError;         // why it is being asked for again
    QWidget *m_loginPrompt = nullptr;
    QLabel *m_loginLabel = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
};
