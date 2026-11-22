/*
 * puddlejumper — a modern terminal built on Qt Quick and QMLTermWidget.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settings_store.h"

SettingsStore::SettingsStore(QObject *parent)
    : QObject(parent)
    , m_settings()
{
    // QSettings is synchronous but the platform may cache; make sure nothing
    // is left unwritten if the process dies unexpectedly.
    m_settings.setFallbacksEnabled(false);
}

QString SettingsStore::stringValue(const QString &key, const QString &fallback) const
{
    return m_settings.value(key, fallback).toString();
}

int SettingsStore::intValue(const QString &key, int fallback) const
{
    return m_settings.value(key, fallback).toInt();
}

double SettingsStore::doubleValue(const QString &key, double fallback) const
{
    return m_settings.value(key, fallback).toDouble();
}

bool SettingsStore::boolValue(const QString &key, bool fallback) const
{
    return m_settings.value(key, fallback).toBool();
}

bool SettingsStore::contains(const QString &key) const
{
    return m_settings.contains(key);
}

void SettingsStore::setString(const QString &key, const QString &value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
}

void SettingsStore::setInt(const QString &key, int value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
}

void SettingsStore::setDouble(const QString &key, double value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
}

void SettingsStore::setBool(const QString &key, bool value)
{
    m_settings.setValue(key, value);
    m_settings.sync();
}

void SettingsStore::remove(const QString &key)
{
    m_settings.remove(key);
    m_settings.sync();
}

void SettingsStore::saveWindow(int x, int y, int width, int height, bool maximized)
{
    m_settings.setValue(QStringLiteral("window/x"), x);
    m_settings.setValue(QStringLiteral("window/y"), y);
    m_settings.setValue(QStringLiteral("window/width"), width);
    m_settings.setValue(QStringLiteral("window/height"), height);
    m_settings.setValue(QStringLiteral("window/maximized"), maximized);
    m_settings.sync();
}

bool SettingsStore::storedWindowMaximized() const
{
    return m_settings.value(QStringLiteral("window/maximized"), false).toBool();
}

QString SettingsStore::filePath() const
{
    return m_settings.fileName();
}
