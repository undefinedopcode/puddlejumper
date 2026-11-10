/*
 * superterm — a modern terminal built on Qt Quick and QMLTermWidget.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QObject>
#include <QSettings>
#include <QStringList>

/*
 * Thin QML-facing wrapper over QSettings.
 *
 * Values are stored in the standard platform location, keyed by the
 * application/organisation name set in main.cpp:
 *   Linux   ~/.config/superterm/superterm.conf
 *   macOS   ~/Library/Preferences/com.superterm.superterm.plist
 *
 * Every write is flushed immediately, so an unclean exit cannot lose
 * settings.
 */
class SettingsStore : public QObject
{
    Q_OBJECT

public:
    explicit SettingsStore(QObject *parent = nullptr);

    // Scalars. Each getter takes a fallback used when the key is absent.
    Q_INVOKABLE QString stringValue(const QString &key,
                                    const QString &fallback = QString()) const;
    Q_INVOKABLE int intValue(const QString &key, int fallback = 0) const;
    Q_INVOKABLE double doubleValue(const QString &key, double fallback = 0.0) const;
    Q_INVOKABLE bool boolValue(const QString &key, bool fallback = false) const;
    Q_INVOKABLE bool contains(const QString &key) const;

    Q_INVOKABLE void setString(const QString &key, const QString &value);
    Q_INVOKABLE void setInt(const QString &key, int value);
    Q_INVOKABLE void setDouble(const QString &key, double value);
    Q_INVOKABLE void setBool(const QString &key, bool value);
    Q_INVOKABLE void remove(const QString &key);

    // Convenience for the common "restore my geometry" case.
    Q_INVOKABLE void saveWindow(int x, int y, int width, int height, bool maximized);
    Q_INVOKABLE bool storedWindowMaximized() const;

    Q_INVOKABLE QString filePath() const;

private:
    mutable QSettings m_settings;
};
