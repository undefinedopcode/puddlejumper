/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "notifier.h"

#include <QApplication>

#if defined(PUDDLEJUMPER_DBUS)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>
#elif !defined(Q_OS_MACOS)
#include <QSystemTrayIcon>
#include <QTimer>
#endif

#if defined(PUDDLEJUMPER_DBUS)

namespace {
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");

// Calls are sent without waiting (and without QDBusInterface, which asks
// the service to describe itself, waiting, when made).
QDBusMessage call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kService, method);
    message.setArguments(arguments);
    return message;
}
}

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, QStringLiteral("ActionInvoked"),
                this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kService, QStringLiteral("NotificationClosed"),
                this, SLOT(onClosed(uint, uint)));
}

Notifier::~Notifier() = default;

void Notifier::notify(const QString &key, const QString &title, const QString &body)
{
    // The default action is a click on the notification itself.
    const QStringList actions{QStringLiteral("default"), tr("Show")};
    const QVariantMap hints{
        {QStringLiteral("desktop-entry"), QStringLiteral("puddlejumper")},
        {QStringLiteral("category"), QStringLiteral("im.received")},
    };
    const QDBusMessage message = call(QStringLiteral("Notify"), {
        QApplication::applicationDisplayName(), m_ids.value(key, 0u), QStringLiteral("puddlejumper"),
        title, body.toHtmlEscaped(),   // servers may read markup in the body
        actions, hints, -1});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, key](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isValid())
            m_ids.insert(key, reply.value());
        w->deleteLater();
    });
}

void Notifier::withdraw(const QString &key)
{
    const uint id = m_ids.take(key);
    if (id)
        QDBusConnection::sessionBus().asyncCall(call(QStringLiteral("CloseNotification"), {id}));
}

void Notifier::onActionInvoked(uint id, const QString &action)
{
    const QString key = m_ids.key(id);
    if (!key.isEmpty() && action == QLatin1String("default"))
        emit activated(key);
}

void Notifier::onClosed(uint id, uint)
{
    m_ids.remove(m_ids.key(id));
}

#elif !defined(Q_OS_MACOS)

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
}

Notifier::~Notifier() = default;

void Notifier::notify(const QString &key, const QString &title, const QString &body)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    // A tray icon only while there's something to say.
    if (!m_tray) {
        m_tray = new QSystemTrayIcon(QApplication::windowIcon(), this);
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
            emit activated(m_trayKey);
            m_tray->hide();
        });
        connect(m_tray, &QSystemTrayIcon::activated, this, [this]() {
            emit activated(m_trayKey);
            m_tray->hide();
        });
    }
    m_trayKey = key;
    m_tray->show();
    m_tray->showMessage(title, body, QSystemTrayIcon::Information);
}

void Notifier::withdraw(const QString &key)
{
    if (m_tray && key == m_trayKey)
        m_tray->hide();
}

#endif
