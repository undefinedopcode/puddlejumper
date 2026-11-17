/*
 * probe-vnc — drives VncView against a live server, for development.
 *
 *   QT_QPA_PLATFORM=offscreen ./build/probe-vnc localhost:5900 /tmp/vnc [keys...]
 *
 * Connects, waits for the screen to settle, and saves <prefix>-0.png. Each
 * further argument is then pressed and released (a Qt key name such as
 * "Meta+Space", or "text:hello" to type it) and the screen is saved again as
 * <prefix>-<n>.png. The events are synthetic, so they take VncView's
 * no-native-code path.
 *
 * PROBE_VNC_PASSWORDS="wrong,right" answers the login prompt with each in
 * turn, as typed into it, and PROBE_VNC_USER fills its name field if shown.
 * PROBE_VNC_16BIT asks for 16-bit colour.
 */

#include "../vncclient.h"
#include "../vncview.h"
#ifdef SUPERTERM_RDP
#include "../rdpclient.h"
#endif

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QAbstractButton>
#include <QClipboard>
#include <QFocusEvent>
#include <QMessageBox>
#include <QLabel>
#include <QLineEdit>
#include <QHash>
#include <QTimer>

#include <algorithm>

#include <cstdio>

namespace {

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void key(QWidget *w, QEvent::Type type, int key, Qt::KeyboardModifiers mods, const QString &text = {})
{
    QKeyEvent event(type, key, mods, text);
    QCoreApplication::sendEvent(w, &event);
}

void press(QWidget *w, const QString &spec)
{
    if (spec.startsWith(QLatin1String("wait:"))) {
        wait(spec.mid(5).toInt() * 1000);
        return;
    }
    if (spec.startsWith(QLatin1String("resize:"))) {
        // resize:WxH, the view itself (an RDP desktop follows)
        const QStringList wh = spec.mid(7).split(QLatin1Char('x'));
        w->resize(wh.value(0).toInt(), wh.value(1).toInt());
        wait(3000);
        return;
    }
    if (spec.startsWith(QLatin1String("clip:"))) {
        // clip:text becomes the clipboard here, handed over as on focus
        QGuiApplication::clipboard()->setText(spec.mid(5));
        QFocusEvent out(QEvent::FocusOut), in(QEvent::FocusIn);
        QCoreApplication::sendEvent(w, &out);
        QCoreApplication::sendEvent(w, &in);
        wait(500);
        return;
    }
    if (spec.startsWith(QLatin1String("click:"))) {
        // click:x,y in the view's own coordinates
        const QStringList xy = spec.mid(6).split(QLatin1Char(','));
        const QPointF pos(xy.value(0).toInt(), xy.value(1).toInt());
        for (QEvent::Type type : {QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
            QMouseEvent event(type, pos, w->mapToGlobal(pos), button,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(w, &event);
            wait(60);
        }
        return;
    }
    if (spec.startsWith(QLatin1String("text:"))) {
        for (QChar c : spec.mid(5)) {
            const int k = c.toUpper().unicode();
            key(w, QEvent::KeyPress, k, Qt::NoModifier, c);
            key(w, QEvent::KeyRelease, k, Qt::NoModifier, c);
            wait(30);
        }
        return;
    }
    const QKeyCombination combo = QKeySequence(spec)[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    QList<int> modKeys;
    if (mods & Qt::ShiftModifier) modKeys << Qt::Key_Shift;
    if (mods & Qt::ControlModifier) modKeys << Qt::Key_Control;
    if (mods & Qt::AltModifier) modKeys << Qt::Key_Alt;
    if (mods & Qt::MetaModifier) modKeys << Qt::Key_Meta;
    for (int m : modKeys)
        key(w, QEvent::KeyPress, m, mods);
    key(w, QEvent::KeyPress, combo.key(), mods);
    key(w, QEvent::KeyRelease, combo.key(), mods);
    for (int m : modKeys)
        key(w, QEvent::KeyRelease, m, mods);
}

} // namespace

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: probe-vnc host:port prefix [keys...]\n");
        return 2;
    }
    QString host;
    quint16 port;
    if (!VncView::parseAddress(args.at(1), &host, &port)) {
        std::fprintf(stderr, "bad address\n");
        return 2;
    }
    const QString prefix = args.at(2);

    // PROBE_RDP: the address is an RDP server's (as given: no display numbers).
    RemoteClient *client = nullptr;
#ifdef SUPERTERM_RDP
    if (qEnvironmentVariableIsSet("PROBE_RDP")) {
        client = new RdpClient;
        port = quint16(args.at(1).section(QLatin1Char(':'), -1).toInt());
    }
#endif
    VncView view(nullptr, client);
    view.resize(1280, 800);
    view.show();
    int updates = 0;
    QElapsedTimer lastUpdate;
    qint64 area = 0;
    QHash<QString, int> shapes;
    QObject::connect(view.client(), &RemoteClient::updated, [&](const QRect &r) {
        ++updates;
        area += qint64(r.width()) * r.height();
        ++shapes[QStringLiteral("%1x%2@%3,%4").arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y())];
        lastUpdate.start();
    });
    QStringList passwords = qEnvironmentVariable("PROBE_VNC_PASSWORDS").split(QLatin1Char(','),
                                                                             Qt::SkipEmptyParts);
    QString failure;
    bool closed = false;
    QObject::connect(view.client(), &RemoteClient::cursorChanged, [&](const QImage &image, const QPoint &hot) {
        std::printf("cursor: %dx%d hotspot %d,%d\n", image.width(), image.height(), hot.x(), hot.y());
        if (!image.isNull())
            image.save(prefix + QStringLiteral("-cursor.png"));
    });
    QObject::connect(view.client(), &RemoteClient::resized, [&](const QSize &size) {
        std::printf("resized: %dx%d\n", size.width(), size.height());
    });
    QObject::connect(view.client(), &RemoteClient::clipboardReceived, [&](const QString &text) {
        std::printf("clipboard: \"%s\"\n", qPrintable(text));
    });
    QObject::connect(view.client(), &RemoteClient::disconnected, [&](const QString &reason) {
        std::printf("disconnected: %s\n", qPrintable(reason));
        // A wrong password is asked for again, not the end.
        if (view.client()->authenticationFailed())
            return;
        failure = reason;
        closed = true;
    });
    // The certificate question, answered as a user would: PROBE_RDP_CERT=reject
    // turns it down, anything else trusts.
    QObject::connect(view.client(), &RemoteClient::certificateRequested, &view,
                     [&](const QString &name, const QString &fingerprint, bool changed) {
        std::printf("certificate: name=\"%s\" sha256=%s changed=%d\n", qPrintable(name),
                    qPrintable(fingerprint), changed);
        QTimer::singleShot(100, &view, [&]() {
            auto *box = view.findChild<QMessageBox *>();
            if (!box)
                return;
            box->grab().save(prefix + QStringLiteral("-certificate.png"));
            const bool reject = qgetenv("PROBE_RDP_CERT") == "reject";
            std::printf("certificate: %s\n", reject ? "rejecting" : "trusting");
            if (reject)
                box->reject();
            else if (auto *trust = box->findChild<QAbstractButton *>(QStringLiteral("trust")))
                trust->click();
        });
    });
    QObject::connect(view.client(), &RemoteClient::credentialsRequested, &view, [&]() {
        // The view shows its prompt first; answer it as a user would.
        QTimer::singleShot(100, &view, [&]() {
            auto *label = view.findChild<QLabel *>();
            auto *user = view.findChild<QLineEdit *>(QStringLiteral("username"));
            auto *edit = view.findChild<QLineEdit *>(QStringLiteral("password"));
            std::printf("prompt: %s\n", qPrintable(label ? label->text().replace(QLatin1Char('\n'), QLatin1String(" / ")) : QString()));
            if (!edit || passwords.isEmpty() || !edit->isVisible()) {
                view.grab().save(prefix + QStringLiteral("-prompt.png"));   // as left unanswered
                return;
            }
            const QString password = passwords.takeFirst();
            if (user && user->isVisible()) {
                user->setText(qEnvironmentVariable("PROBE_VNC_USER"));
                std::printf("typing user \"%s\"\n", qPrintable(user->text()));
            }
            std::printf("typing password \"%s\"\n", qPrintable(password));
            edit->setText(password);
            emit edit->returnPressed();
        });
    });
    QElapsedTimer total;
    total.start();
    if (qEnvironmentVariableIsSet("PROBE_VNC_16BIT"))
        view.client()->setReducedColour(true);
    view.connectToHost(host, port);

    // Up, and no updates for a moment (or 10 s at most).
    while (!closed && total.elapsed() < 10000
           && !(updates > 0 && lastUpdate.elapsed() > 700))
        wait(50);
    if (closed) {
        std::fprintf(stderr, "disconnected: %s\n", qPrintable(failure));
        return 1;
    }
    const QImage &fb = view.client()->framebuffer();
    std::printf("connected: name=\"%s\" size=%dx%d scanCodes=%d updates=%d in %lld ms\n",
                qPrintable(view.client()->desktopName()), fb.width(), fb.height(),
                qobject_cast<VncClient *>(view.client())
                    && static_cast<VncClient *>(view.client())->hasScanCodes(),
                updates, total.elapsed());
    int lit = 0;
    for (int y = 0; y < fb.height(); y += 4)
        for (int x = 0; x < fb.width(); x += 4)
            lit += (fb.pixel(x, y) & 0xffffff) != 0;
    std::printf("framebuffer: %d of %d sampled pixels not black\n", lit,
                (fb.width() / 4) * (fb.height() / 4));
    fb.save(prefix + QStringLiteral("-fb.png"));
    view.grab().save(prefix + QStringLiteral("-0.png"));

    view.setFocus();
    for (int i = 3; i < args.size(); ++i) {
        press(&view, args.at(i));
        wait(1200);
        view.grab().save(prefix + QStringLiteral("-%1.png").arg(i - 2));
        std::printf("pressed %s (updates so far %d, %lld pixels)\n", qPrintable(args.at(i)), updates,
                    area);
        if (args.at(i).startsWith(QLatin1String("wait:"))) {
            QList<QPair<int, QString>> top;
            for (auto it = shapes.cbegin(); it != shapes.cend(); ++it)
                top.append({it.value(), it.key()});
            std::sort(top.begin(), top.end(), std::greater<>());
            for (int t = 0; t < qMin(5, int(top.size())); ++t)
                std::printf("  %5d x %s\n", top.at(t).first, qPrintable(top.at(t).second));
        }
    }
    return 0;
}
