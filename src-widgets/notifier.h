/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QSystemTrayIcon;

/*
 * Desktop notifications: the freedesktop notification service over D-Bus on
 * Linux and the BSDs, the notification centre on macOS (in an app bundle; a
 * bare binary can't have them), a tray message elsewhere.
 *
 * Each notification has a key: a newer one with the same key replaces it,
 * and clicking it reports the key.
 */
class Notifier : public QObject
{
    Q_OBJECT

public:
    explicit Notifier(QObject *parent = nullptr);
    ~Notifier() override;

    void notify(const QString &key, const QString &title, const QString &body);
    /* Take the notification away (what it was about has been seen). */
    void withdraw(const QString &key);

signals:
    /* The user clicked the notification. */
    void activated(const QString &key);

#if defined(PUDDLEJUMPER_DBUS)
private slots:
    // The service's signals, connected by name.
    void onActionInvoked(uint id, const QString &action);
    void onClosed(uint id, uint reason);

private:
    QHash<QString, uint> m_ids;   // key -> the service's id for it
#elif defined(Q_OS_MACOS)
private:
    // In notifier_mac.mm: the notification centre's delegate.
    void *m_delegate = nullptr;
#else
private:
    QSystemTrayIcon *m_tray = nullptr;
    QString m_trayKey;            // what the tray's message is about
#endif
};
