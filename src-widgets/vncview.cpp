/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "vncview.h"

#include "formername.h"
#include "vncclient.h"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/*
 * XT (set 1) scan codes, 0xE0nn for the extended ones, from what the
 * platform reports for a key: the macOS virtual key code, Windows' scan
 * code, or the X/xkb keycode (evdev + 8) on Linux under X11 and Wayland alike.
 */
#ifdef Q_OS_MACOS
quint32 scanCodeForNative(quint32 kvk)
{
    static const quint16 table[128] = {
        // 0x00: A S D F H G Z X C V § B Q W E R
        0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2c, 0x2d, 0x2e, 0x2f, 0x56, 0x30, 0x10, 0x11, 0x12, 0x13,
        // 0x10: Y T 1 2 3 4 6 5 = 9 7 - 8 0 ] O
        0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06, 0x0d, 0x0a, 0x08, 0x0c, 0x09, 0x0b, 0x1b, 0x18,
        // 0x20: U [ I P Return L J ' K ; \ , / N M .
        0x16, 0x1a, 0x17, 0x19, 0x1c, 0x26, 0x24, 0x28, 0x25, 0x27, 0x2b, 0x33, 0x35, 0x31, 0x32, 0x34,
        // 0x30: Tab Space ` Backspace (Enter) Esc RCmd Cmd Shift Caps Option Control RShift ROption RControl Fn
        0x0f, 0x39, 0x29, 0x0e, 0xe01c, 0x01, 0xe05c, 0xe05b, 0x2a, 0x3a, 0x38, 0x1d, 0x36, 0xe038, 0xe01d, 0,
        // 0x40: F17 KP. - KP* - KP+ - Clear VolUp VolDown Mute KP/ KPEnter - KP- F18
        0, 0x53, 0, 0x37, 0, 0x4e, 0, 0x45, 0xe030, 0xe02e, 0xe020, 0xe035, 0xe01c, 0, 0x4a, 0,
        // 0x50: F19 KP= KP0 KP1 KP2 KP3 KP4 KP5 KP6 KP7 F20 KP8 KP9 Yen _ KP,
        0, 0x59, 0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0, 0x48, 0x49, 0x7d, 0x73, 0x7e,
        // 0x60: F5 F6 F7 F3 F8 F9 Eisu F11 Kana F13 F16 F14 - F10 - F12
        0x3f, 0x40, 0x41, 0x3d, 0x42, 0x43, 0x7b, 0x57, 0x70, 0x64, 0x67, 0x65, 0, 0x44, 0, 0x58,
        // 0x70: - F15 Help Home PgUp FwdDel F4 End F2 PgDn F1 Left Right Down Up -
        0, 0x66, 0xe052, 0xe047, 0xe049, 0xe053, 0x3e, 0xe04f, 0x3c, 0xe051, 0x3b, 0xe04b, 0xe04d, 0xe050, 0xe048, 0,
    };
    return kvk < 128 ? table[kvk] : 0;
}
#elif defined(Q_OS_WIN)
quint32 scanCodeForNative(quint32 code)
{
    // The XT code itself, the extended ones marked by bit 8.
    return (code & 0x100) ? 0xe000 | (code & 0xff) : code & 0xff;
}
#else
quint32 scanCodeForNative(quint32 xkbKeycode)
{
    if (xkbKeycode < 8)
        return 0;
    const quint32 evdev = xkbKeycode - 8;
    // Up to F12 the evdev codes are the XT ones, but for one.
    if (evdev <= 88 && evdev != 85)
        return evdev;
    switch (evdev) {
    case 85: return 0x76;     // Zenkaku/Hankaku
    case 89: return 0x73;     // Ro
    case 90: return 0x78;     // Katakana
    case 91: return 0x77;     // Hiragana
    case 92: return 0x79;     // Henkan
    case 93: return 0x70;     // Katakana/Hiragana
    case 94: return 0x7b;     // Muhenkan
    case 95: return 0x5c;     // keypad JP comma
    case 96: return 0xe01c;   // keypad Enter
    case 97: return 0xe01d;   // right Ctrl
    case 98: return 0xe035;   // keypad /
    case 99: return 0xe037;   // SysRq
    case 100: return 0xe038;  // right Alt
    case 102: return 0xe047;  // Home
    case 103: return 0xe048;  // Up
    case 104: return 0xe049;  // Page Up
    case 105: return 0xe04b;  // Left
    case 106: return 0xe04d;  // Right
    case 107: return 0xe04f;  // End
    case 108: return 0xe050;  // Down
    case 109: return 0xe051;  // Page Down
    case 110: return 0xe052;  // Insert
    case 111: return 0xe053;  // Delete
    case 113: return 0xe020;  // Mute
    case 114: return 0xe02e;  // Volume Down
    case 115: return 0xe030;  // Volume Up
    case 117: return 0x59;    // keypad =
    case 119: return 0xe046;  // Pause
    case 121: return 0x7e;    // keypad comma
    case 124: return 0x7d;    // Yen
    case 125: return 0xe05b;  // left Super
    case 126: return 0xe05c;  // right Super
    case 127: return 0xe05d;  // Menu
    default: return 0;
    }
}
#endif

// The scan code from Qt's key, for events with no native code (synthetic ones).
quint32 scanCodeForKey(int key)
{
    static const quint8 letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return letters[key - Qt::Key_A];
    if (key >= Qt::Key_1 && key <= Qt::Key_9)
        return 0x02 + (key - Qt::Key_1);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F10)
        return 0x3b + (key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_0: return 0x0b;
    case Qt::Key_F11: return 0x57;
    case Qt::Key_F12: return 0x58;
    case Qt::Key_Escape: return 0x01;
    case Qt::Key_Minus: return 0x0c;
    case Qt::Key_Equal: return 0x0d;
    case Qt::Key_Backspace: return 0x0e;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 0x0f;
    case Qt::Key_BracketLeft: return 0x1a;
    case Qt::Key_BracketRight: return 0x1b;
    case Qt::Key_Return: return 0x1c;
    case Qt::Key_Enter: return 0xe01c;
    case Qt::Key_Semicolon: return 0x27;
    case Qt::Key_Apostrophe: return 0x28;
    case Qt::Key_QuoteLeft: return 0x29;
    case Qt::Key_Backslash: return 0x2b;
    case Qt::Key_Comma: return 0x33;
    case Qt::Key_Period: return 0x34;
    case Qt::Key_Slash: return 0x35;
    case Qt::Key_Space: return 0x39;
    case Qt::Key_CapsLock: return 0x3a;
    case Qt::Key_Shift: return 0x2a;
    case Qt::Key_Alt: return 0x38;
    case Qt::Key_AltGr: return 0xe038;
#ifdef Q_OS_MACOS
    // Qt's Control is the Command key, and its Meta the Control key.
    case Qt::Key_Control: return 0xe05b;
    case Qt::Key_Meta: return 0x1d;
#else
    case Qt::Key_Control: return 0x1d;
    case Qt::Key_Meta: case Qt::Key_Super_L: return 0xe05b;
    case Qt::Key_Super_R: return 0xe05c;
#endif
    case Qt::Key_Menu: return 0xe05d;
    case Qt::Key_Insert: return 0xe052;
    case Qt::Key_Delete: return 0xe053;
    case Qt::Key_Home: return 0xe047;
    case Qt::Key_End: return 0xe04f;
    case Qt::Key_PageUp: return 0xe049;
    case Qt::Key_PageDown: return 0xe051;
    case Qt::Key_Left: return 0xe04b;
    case Qt::Key_Right: return 0xe04d;
    case Qt::Key_Up: return 0xe048;
    case Qt::Key_Down: return 0xe050;
    default: return 0;
    }
}

// The X keysym, for servers that only take those.
quint32 keysymFor(const QKeyEvent *event)
{
    const int key = event->key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return 0xffbe + (key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 0xff09;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return 0xff8d;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_Shift: return 0xffe1;
    case Qt::Key_CapsLock: return 0xffe5;
    case Qt::Key_Alt: return 0xffe9;
    case Qt::Key_AltGr: return 0xfe03;
    case Qt::Key_Menu: return 0xff67;
#ifdef Q_OS_MACOS
    case Qt::Key_Control: return 0xffeb;   // Command: Super
    case Qt::Key_Meta: return 0xffe3;
#else
    case Qt::Key_Control: return 0xffe3;
    case Qt::Key_Meta: case Qt::Key_Super_L: return 0xffeb;
    case Qt::Key_Super_R: return 0xffec;
#endif
    default: break;
    }
    // Characters: Latin-1 keysyms are the code points, the rest are offset.
    const QString text = event->text();
    if (text.size() == 1 && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7f) {
        const char16_t c = text.at(0).unicode();
        return c < 0x100 ? c : 0x01000000u | c;
    }
    // Ctrl+letter and the like: the key's own, unshifted.
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'a' + (key - Qt::Key_A);
    if (key >= 0x20 && key < 0x7f)
        return quint32(key);
    return 0;
}

// The local pointer while the remote end draws its own: a small dot.
QCursor dotCursor()
{
    QImage dot(5, 5, QImage::Format_ARGB32_Premultiplied);
    dot.fill(Qt::transparent);
    QPainter p(&dot);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(0.5, 0.5, 4, 4));
    p.end();
    return QCursor(QPixmap::fromImage(dot), 2, 2);
}

// After a click, the waits between prods of the server (see the
// constructor): often at first, while things open, then less (1.5 s in all).
constexpr int kNudgeDelays[] = {80, 150, 250, 400, 620};

} // namespace

VncView::VncView(QWidget *parent, RemoteClient *client)
    : QWidget(parent), m_client(client ? client : new VncClient(this))
{
    m_client->setParent(this);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);   // keys, not composed text
    setCursor(dotCursor());

    /*
     * A Mac's Screen Sharing looks at the screen again when the pointer
     * moves, and otherwise only now and then: what a click brings up a moment
     * later (a menu fading in, a window opening) can take a second or two to
     * be sent. So for a while after a click, the pointer is moved a pixel over
     * and back now and then (the pointer sent again where it is, or a part of
     * the screen asked for afresh, aren't enough). PUDDLEJUMPER_VNC_NUDGE=off
     * stops it.
     */
    m_nudge = m_client->scheme() == QLatin1String("vnc") && appEnv("VNC_NUDGE") != "off";
    m_sizeTimer = new QTimer(this);
    m_sizeTimer->setSingleShot(true);
    m_sizeTimer->setInterval(300);
    connect(m_sizeTimer, &QTimer::timeout, this, &VncView::wantDesktopSize);
    m_nudgeTimer = new QTimer(this);
    m_nudgeTimer->setSingleShot(true);
    connect(m_nudgeTimer, &QTimer::timeout, this, &VncView::nudge);

    // The login prompt, shown over the view when the server asks.
    auto *prompt = new QFrame(this);
    prompt->setFrameShape(QFrame::StyledPanel);
    prompt->setAutoFillBackground(true);
    prompt->setCursor(Qt::ArrowCursor);
    m_loginLabel = new QLabel(prompt);
    m_loginLabel->setWordWrap(true);
    m_usernameEdit = new QLineEdit(prompt);
    m_usernameEdit->setObjectName(QStringLiteral("username"));
    m_usernameEdit->setPlaceholderText(tr("User name"));
    m_passwordEdit = new QLineEdit(prompt);
    m_passwordEdit->setObjectName(QStringLiteral("password"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Password"));
    m_passwordEdit->setMinimumWidth(220);
    auto *connectButton = new QPushButton(tr("Connect"), prompt);
    auto *row = new QHBoxLayout;
    row->addWidget(m_passwordEdit, 1);
    row->addWidget(connectButton);
    auto *column = new QVBoxLayout(prompt);
    column->addWidget(m_loginLabel);
    column->addWidget(m_usernameEdit);
    column->addLayout(row);
    connect(m_usernameEdit, &QLineEdit::returnPressed, m_passwordEdit,
            qOverload<>(&QWidget::setFocus));
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &VncView::submitLogin);
    connect(connectButton, &QPushButton::clicked, this, &VncView::submitLogin);
    prompt->hide();
    m_loginPrompt = prompt;

    connect(m_client, &RemoteClient::credentialsRequested, this, [this](bool needsUsername) {
        if (!m_password.isNull())
            m_client->setCredentials(m_username, m_password);   // reconnecting
        else
            showLoginPrompt(needsUsername, m_loginError);
    });
    connect(m_client, &RemoteClient::certificateRequested, this,
            [this](const QString &name, const QString &fingerprint, bool changed) {
        const QString what = changed
            ? tr("<b>The certificate of %1 is not the one trusted before.</b><br>It may have "
                 "been renewed, or something may be intercepting the connection.").arg(m_address)
            : tr("%1 identifies itself with a certificate that can't be verified, as is usual "
                 "for a Windows machine's own.").arg(m_address);
        auto *box = new QMessageBox(QMessageBox::Warning, tr("Remote Desktop"),
                                    what + QStringLiteral("<br><br>")
                                        + tr("Name: %1<br>SHA-256 fingerprint:<br><tt>%2</tt>")
                                              .arg(name.toHtmlEscaped(), fingerprint)
                                        + QStringLiteral("<br><br>")
                                        + tr("Trust it, and remember it for this address?"),
                                    QMessageBox::NoButton, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        QPushButton *trust = box->addButton(tr("Trust"), QMessageBox::AcceptRole);
        trust->setObjectName(QStringLiteral("trust"));
        box->setDefaultButton(box->addButton(QMessageBox::Cancel));
        connect(box, &QMessageBox::finished, this, [this, box, trust]() {
            m_client->answerCertificate(box->clickedButton() == trust);
        });
        // Gone with the connection, if that ends first.
        connect(m_client, &RemoteClient::disconnected, box, &QDialog::reject);
        box->open();
    });
    connect(m_client, &RemoteClient::connected, this, [this]() {
        m_loginError.clear();
        m_connecting = false;
        setStatus(QString());
        m_sentClipboard.clear();   // a new connection knows nothing of it
        if (hasFocus())
            handOverClipboard();
        emit titleChanged(title());
    });
    connect(m_client, &RemoteClient::resized, this, [this]() {
        updateCursor();
        update();
    });
    connect(m_client, &RemoteClient::updated, this, [this](const QRect &rect) {
        const QRectF box = imageRect();
        const qreal scale = box.width() / m_client->framebuffer().width();
        update(QRectF(box.topLeft() + rect.topLeft() * scale, rect.size() * scale)
                   .toAlignedRect().adjusted(-1, -1, 1, 1));
    });
    connect(m_client, &RemoteClient::cursorChanged, this, [this](const QImage &image, const QPoint &hotspot) {
        m_cursor = image;
        m_cursorHotspot = hotspot;
        updateCursor();
    });
    connect(m_client, &RemoteClient::bell, this, []() { QApplication::beep(); });
    connect(m_client, &RemoteClient::clipboardReceived, this, [this](const QString &text) {
        m_sentClipboard = text;
        QGuiApplication::clipboard()->setText(text);
    });
    connect(m_client, &RemoteClient::disconnected, this, [this](const QString &reason) {
        m_connecting = false;
        m_held.clear();
        m_buttons = 0;
        m_loginPrompt->hide();
        if (m_client->authenticationFailed()) {
            // Ask again, saying why: on a new connection, once this one is
            // done with.
            m_password = QString();
            m_loginError = reason;
            m_connecting = true;
            setStatus(tr("Connecting to %1…").arg(m_address));
            QTimer::singleShot(0, this, &VncView::reconnect);
            return;
        }
        const QString what = reason.isEmpty() ? tr("Disconnected from %1.").arg(m_address)
                                              : tr("Disconnected from %1: %2").arg(m_address, reason);
        setStatus(what + QLatin1Char('\n') + tr("Click or press Return to connect again."));
        emit titleChanged(title());
    });
}

void VncView::connectToHost(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
    m_connecting = true;
    m_address = host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]:%2").arg(host).arg(port)
                                                 : QStringLiteral("%1:%2").arg(host).arg(port);
    setStatus(tr("Connecting to %1…").arg(m_address));
    emit titleChanged(title());
    wantDesktopSize();
    m_client->connectToHost(host, port);
}

QString VncView::title() const
{
    if (m_client->isConnected() && !m_client->desktopName().isEmpty())
        return m_client->desktopName();
    return m_address;
}

QSize VncView::sizeHint() const
{
    const QSize size = m_client->framebuffer().size();
    return size.isEmpty() ? QSize(1024, 768) : size;
}

bool VncView::parseAddress(const QString &text, QString *host, quint16 *port)
{
    // host, host:n, [v6]:n, or a bare v6 address.
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*(?:\[([^\]]+)\]|([^:\s]*))(?::(\d+))?\s*$)"));
    const QRegularExpressionMatch m = re.match(text);
    QString h;
    int n = -1;
    if (m.hasMatch()) {
        h = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        if (m.hasCaptured(3) && !m.captured(3).isEmpty())
            n = m.captured(3).toInt();
    } else if (text.count(QLatin1Char(':')) >= 2) {
        h = text.trimmed();
    } else {
        return false;
    }
    if (h.isEmpty())
        h = QStringLiteral("localhost");
    if (n < 0)
        n = 5900;
    else if (n < 100)
        n += 5900;   // a display number
    if (n > 65535)
        return false;
    *host = h;
    *port = quint16(n);
    return true;
}

void VncView::showLoginPrompt(bool needsUsername, const QString &error)
{
    QString text = needsUsername ? tr("Log in to %1 with an account on it.").arg(m_address)
                                 : tr("%1 wants a password.").arg(m_address);
    if (!error.isEmpty()) {
        text = error + QLatin1Char('\n') + text;
        // What usually stops a login a Mac turns down.
        if (needsUsername && m_client->scheme() == QLatin1String("vnc"))
            text += QLatin1Char('\n')
                    + tr("Use the account's short name, and check the Mac lists the account "
                         "itself under Screen Sharing's “Allow access for” (a group such as "
                         "Administrators may not be enough).");
    }
    m_loginLabel->setText(text);
    m_usernameEdit->setVisible(needsUsername);
    m_usernameEdit->setText(m_username);
    m_passwordEdit->clear();
    setStatus(QString());
    m_loginPrompt->show();
    placeLoginPrompt();
    if (hasFocus() || window()->isActiveWindow())
        focusLoginPrompt();
    emit titleChanged(title());
}

void VncView::focusLoginPrompt()
{
    if (m_usernameEdit->isVisible() && m_usernameEdit->text().isEmpty())
        m_usernameEdit->setFocus();
    else
        m_passwordEdit->setFocus();
}

void VncView::submitLogin()
{
    if (m_usernameEdit->isVisible())
        m_username = m_usernameEdit->text();
    m_password = m_passwordEdit->text();
    m_passwordEdit->clear();
    m_loginPrompt->hide();
    setFocus();
    setStatus(tr("Connecting to %1…").arg(m_address));
    m_client->setCredentials(m_username, m_password);
}

void VncView::placeLoginPrompt()
{
    m_loginPrompt->adjustSize();
    const QSize size = m_loginPrompt->size().boundedTo(this->size());
    m_loginPrompt->setGeometry(QRect(QPoint((width() - size.width()) / 2,
                                               (height() - size.height()) / 2), size));
}

void VncView::setReducedColour(bool on)
{
    if (on == m_client->reducedColour())
        return;
    m_client->setReducedColour(on);
    // The format is settled when connecting: a connection being set up
    // picks it up, one already up starts again with it.
    if (m_client->isConnected())
        reconnect();
}

void VncView::setAdaptiveQuality(bool on)
{
    if (on == m_client->adaptiveQuality())
        return;
    m_client->setAdaptiveQuality(on);
    // Chosen when connecting, like the colour depth.
    if (m_client->isConnected())
        reconnect();
}

void VncView::reconnect()
{
    if (!m_host.isEmpty())
        connectToHost(m_host, m_port);
}

QRectF VncView::imageRect() const
{
    const QSizeF image = m_client->framebuffer().size();
    if (image.isEmpty())
        return QRectF();
    const qreal scale = std::min({1.0, width() / image.width(), height() / image.height()});
    const QSizeF size = image * scale;
    return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

QPoint VncView::toRemote(const QPointF &pos) const
{
    const QRectF box = imageRect();
    if (box.isEmpty())
        return QPoint();
    const qreal scale = m_client->framebuffer().width() / box.width();
    return ((pos - box.topLeft()) * scale).toPoint();
}

void VncView::setStatus(const QString &status)
{
    m_status = status;
    update();
}

void VncView::updateCursor()
{
    if (m_cursor.isNull()) {
        setCursor(dotCursor());
        return;
    }
    // At the screen's scale, so it matches what it points at.
    const QRectF box = imageRect();
    const qreal scale = box.isEmpty() ? 1.0 : box.width() / m_client->framebuffer().width();
    const QSize size = (QSizeF(m_cursor.size()) * scale).toSize().expandedTo(QSize(1, 1));
    setCursor(QCursor(QPixmap::fromImage(m_cursor.scaled(size, Qt::IgnoreAspectRatio,
                                                         Qt::SmoothTransformation)),
                      qRound(m_cursorHotspot.x() * scale), qRound(m_cursorHotspot.y() * scale)));
}

void VncView::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    p.fillRect(event->rect(), Qt::black);
    const QImage &image = m_client->framebuffer();
    if (!image.isNull() && m_client->isConnected()) {
        const QRectF box = imageRect();
        if (box.size() != QSizeF(image.size()))
            p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(box, image);
    }
    if (!m_status.isEmpty()) {
        p.setPen(QColor(0xc0, 0xc0, 0xc0));
        p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, m_status);
    }
}

void VncView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_loginPrompt->isVisible())
        placeLoginPrompt();
    updateCursor();
    m_sizeTimer->start();
}

void VncView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_sizeTimer->start();   // for a size taken while hidden
}

void VncView::wantDesktopSize()
{
    // Where the protocol can ask (RDP), the remote desktop is the view's
    // size, pixel for pixel. Not while hidden: a tab behind another has
    // whatever size it was left with.
    if (!isVisible() || width() < 100 || height() < 100)
        return;
    const qreal scale = devicePixelRatioF();
    m_client->setDesktopSize((QSizeF(size()) * scale).toSize(), scale);
}

void VncView::handOverClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty() && text != m_sentClipboard) {
        m_sentClipboard = text;
        m_client->sendClipboard(text);
    }
}

/*
 * Mouse
 */

void VncView::sendPointer(const QPointF &pos, Qt::MouseButtons buttons)
{
    int mask = 0;
    if (buttons & Qt::LeftButton)
        mask |= 1;
    if (buttons & Qt::MiddleButton)
        mask |= 2;
    if (buttons & Qt::RightButton)
        mask |= 4;
    m_buttons = mask;
    m_lastPos = toRemote(pos);
    m_client->sendPointer(m_lastPos, mask);
}

void VncView::mousePressEvent(QMouseEvent *event)
{
    if (!m_client->isConnected()) {
        if (event->button() == Qt::LeftButton && !m_connecting)
            reconnect();
        return;
    }
    sendPointer(event->position(), event->buttons());
    startNudging();
}

void VncView::mouseReleaseEvent(QMouseEvent *event)
{
    sendPointer(event->position(), event->buttons());
    startNudging();
}

void VncView::startNudging()
{
    if (!m_nudge)
        return;
    m_nudgeStep = 0;
    m_nudgeTimer->start(kNudgeDelays[0]);
}

void VncView::nudge()
{
    if (m_client->isConnected()) {
        m_client->sendPointer(m_lastPos + QPoint(m_lastPos.x() > 0 ? -1 : 1, 0), m_buttons);
        m_client->sendPointer(m_lastPos, m_buttons);
    }
    if (++m_nudgeStep < int(std::size(kNudgeDelays)))
        m_nudgeTimer->start(kNudgeDelays[m_nudgeStep]);
}

void VncView::mouseMoveEvent(QMouseEvent *event)
{
    sendPointer(event->position(), event->buttons());
}

void VncView::wheelEvent(QWheelEvent *event)
{
    m_lastPos = toRemote(event->position());
    // By the distance, where the server takes one (a Mac): a trackpad's in
    // pixels, or a wheel's notch as three lines of ten.
    QPoint distance = event->pixelDelta();
    if (distance.isNull())
        distance = event->angleDelta() / 4;
    if (m_client->sendScroll(m_lastPos, -distance)) {
        event->accept();
        return;
    }
    // Otherwise VNC has wheel clicks only: a press and release of a button per 120.
    m_wheel += event->angleDelta();
    auto click = [this](int button) {
        m_client->sendPointer(m_lastPos, m_buttons | button);
        m_client->sendPointer(m_lastPos, m_buttons);
    };
    for (; m_wheel.y() >= 120; m_wheel.ry() -= 120)
        click(8);
    for (; m_wheel.y() <= -120; m_wheel.ry() += 120)
        click(16);
    for (; m_wheel.x() >= 120; m_wheel.rx() -= 120)
        click(32);
    for (; m_wheel.x() <= -120; m_wheel.rx() += 120)
        click(64);
    event->accept();
}

/*
 * Keyboard
 */

bool VncView::event(QEvent *event)
{
    // Every key is the remote end's while this has the focus: let no
    // shortcut take it first, but those kept for the application.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (!m_client->isConnected() || (m_shortcutFilter && m_shortcutFilter(key)))
            return QWidget::event(event);
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

bool VncView::focusNextPrevChild(bool)
{
    return false;   // Tab and Shift+Tab are keys like any other here
}

void VncView::sendKey(QKeyEvent *event, bool down)
{
    // The remote end repeats a held key itself.
    if (event->isAutoRepeat())
        return;

    // A key is known by its native code where there is one, so its release
    // matches its press whatever the modifiers did to the Qt key between.
#ifdef Q_OS_MACOS
    // (Its key code 0 is A, so a real key is told apart by where it came from.)
    const bool native = event->spontaneous();
    const quint32 nativeCode = event->nativeVirtualKey();
#else
    const bool native = event->nativeScanCode() != 0;
    const quint32 nativeCode = event->nativeScanCode();
#endif
    const quint32 id = native ? 0x80000000u | nativeCode : quint32(event->key());

    if (!down) {
        const auto held = m_held.find(id);
        if (held == m_held.end())
            return;
        m_client->sendKey(false, held->keysym, held->scanCode);
        m_held.erase(held);
        return;
    }

    quint32 scanCode = native ? scanCodeForNative(nativeCode) : 0;
    if (!scanCode)
        scanCode = scanCodeForKey(event->key());
    const quint32 keysym = keysymFor(event);
    if (!scanCode && !keysym)
        return;
    m_held.insert(id, HeldKey{keysym, scanCode});
    m_client->sendKey(true, keysym, scanCode);
}

void VncView::keyPressEvent(QKeyEvent *event)
{
    if (!m_client->isConnected()) {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !m_connecting)
            reconnect();
        else
            QWidget::keyPressEvent(event);
        return;
    }
    sendKey(event, true);
    event->accept();
}

void VncView::keyReleaseEvent(QKeyEvent *event)
{
    sendKey(event, false);
    event->accept();
}

void VncView::releaseKeys()
{
    for (const HeldKey &key : std::as_const(m_held))
        m_client->sendKey(false, key.keysym, key.scanCode);
    m_held.clear();
    if (m_buttons) {
        m_buttons = 0;
        m_client->sendPointer(m_lastPos, 0);
    }
}

void VncView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (m_loginPrompt->isVisible()) {
        focusLoginPrompt();
        return;
    }
    // Hand over what was copied here while away (Latin-1 is all VNC carries).
    handOverClipboard();
}

void VncView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    releaseKeys();
}
