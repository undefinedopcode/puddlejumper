/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settings.h"

#include <QFontDatabase>

namespace {
// Key names match the QML build so an existing config carries over.
constexpr auto kColorScheme = "appearance/colorScheme";
constexpr auto kFontFamily  = "appearance/fontFamily";
constexpr auto kFontSize    = "appearance/fontSize";
constexpr auto kBlink       = "appearance/blinkingCursor";
constexpr auto kBold        = "appearance/boldIntense";
constexpr auto kItalic      = "appearance/italic";
constexpr auto kAntialias   = "appearance/antialias";
constexpr auto kDrawLineChars = "appearance/drawLineChars";
constexpr auto kLineSpacing = "appearance/lineSpacing";
constexpr auto kTabLayout   = "appearance/tabLayout";
constexpr auto kScrollback  = "scrolling/scrollback";
constexpr auto kGeometry    = "window/qtGeometry";
constexpr auto kWinState    = "window/qtState";

// First installed family from the preferred list; the terminal needs a
// fixed-pitch face or its cell grid breaks.
QString resolveMonoFamily()
{
    static const QStringList candidates = {
        QStringLiteral("Hack"),
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Fira Code"),
        QStringLiteral("Iosevka Fixed"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Liberation Mono"),
        QStringLiteral("Noto Sans Mono"),
    };
    const QStringList families = QFontDatabase::families();
    for (const QString &c : candidates) {
        if (families.contains(c, Qt::CaseInsensitive))
            return c;
    }
    return QStringLiteral("monospace");
}
} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    load();
}

void Settings::load()
{
    m_settings.setFallbacksEnabled(false);

    m_appearance.colorScheme =
        m_settings.value(kColorScheme, m_appearance.colorScheme).toString();
    m_appearance.fontFamily =
        m_settings.value(kFontFamily, QString()).toString();
    if (m_appearance.fontFamily.isEmpty())
        m_appearance.fontFamily = resolveMonoFamily();

    m_appearance.fontSize = m_settings.value(kFontSize, m_appearance.fontSize).toDouble();
    m_appearance.blinkingCursor =
        m_settings.value(kBlink, m_appearance.blinkingCursor).toBool();
    m_appearance.boldIntense =
        m_settings.value(kBold, m_appearance.boldIntense).toBool();
    m_appearance.italic = m_settings.value(kItalic, m_appearance.italic).toBool();
    m_appearance.antialias =
        m_settings.value(kAntialias, m_appearance.antialias).toBool();
    m_appearance.drawLineChars =
        m_settings.value(kDrawLineChars, m_appearance.drawLineChars).toBool();
    m_appearance.lineSpacing =
        m_settings.value(kLineSpacing, m_appearance.lineSpacing).toInt();
    m_appearance.tabLayout =
        m_settings.value(kTabLayout, m_appearance.tabLayout).toString();

    m_scrolling.scrollback =
        m_settings.value(kScrollback, m_scrolling.scrollback).toInt();

    // Snap to the half-point grid the UI offers.
    m_appearance.fontSize = qRound(m_appearance.fontSize * 2) / 2.0;
}

void Settings::setColorScheme(const QString &v)
{
    if (m_appearance.colorScheme == v) return;
    m_appearance.colorScheme = v;
    m_settings.setValue(kColorScheme, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setFontFamily(const QString &v)
{
    if (m_appearance.fontFamily == v) return;
    m_appearance.fontFamily = v;
    m_settings.setValue(kFontFamily, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setFontSize(double v)
{
    const double snapped = qRound(v * 2) / 2.0;
    if (qFuzzyCompare(m_appearance.fontSize, snapped)) return;
    m_appearance.fontSize = snapped;
    m_settings.setValue(kFontSize, snapped);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setBlinkingCursor(bool v)
{
    if (m_appearance.blinkingCursor == v) return;
    m_appearance.blinkingCursor = v;
    m_settings.setValue(kBlink, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setBoldIntense(bool v)
{
    if (m_appearance.boldIntense == v) return;
    m_appearance.boldIntense = v;
    m_settings.setValue(kBold, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setItalic(bool v)
{
    if (m_appearance.italic == v) return;
    m_appearance.italic = v;
    m_settings.setValue(kItalic, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setAntialias(bool v)
{
    if (m_appearance.antialias == v) return;
    m_appearance.antialias = v;
    m_settings.setValue(kAntialias, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setDrawLineChars(bool v)
{
    if (m_appearance.drawLineChars == v) return;
    m_appearance.drawLineChars = v;
    m_settings.setValue(kDrawLineChars, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setLineSpacing(int v)
{
    if (m_appearance.lineSpacing == v) return;
    m_appearance.lineSpacing = v;
    m_settings.setValue(kLineSpacing, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setTabLayout(const QString &v)
{
    if (m_appearance.tabLayout == v) return;
    m_appearance.tabLayout = v;
    m_settings.setValue(kTabLayout, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setScrollback(int v)
{
    if (m_scrolling.scrollback == v) return;
    m_scrolling.scrollback = v;
    m_settings.setValue(kScrollback, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::saveWindowGeometry(const QByteArray &geometry, const QByteArray &state)
{
    m_settings.setValue(kGeometry, geometry);
    m_settings.setValue(kWinState, state);
    m_settings.sync();
}

QByteArray Settings::windowGeometry() const
{
    return m_settings.value(kGeometry).toByteArray();
}

QByteArray Settings::windowState() const
{
    return m_settings.value(kWinState).toByteArray();
}
