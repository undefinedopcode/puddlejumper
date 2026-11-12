/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "settings.h"

#include "colorscheme.h"

#include <QDir>
#include <QFile>
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
constexpr auto kOpacity        = "appearance/opacity";
constexpr auto kCursorShape    = "appearance/cursorShape";
constexpr auto kTerminalMargin = "appearance/terminalMargin";
constexpr auto kBidi           = "appearance/bidi";
constexpr auto kScrollback  = "scrolling/scrollback";
constexpr auto kScrollbarPos   = "scrolling/scrollbarPosition";
constexpr auto kFlowCtrlWarn   = "scrolling/flowControlWarning";
constexpr auto kConfirmCloseRunning = "scrolling/confirmCloseWithRunningProcesses";
constexpr auto kMonitorActivity = "scrolling/monitorActivity";
constexpr auto kMonitorSilence  = "scrolling/monitorSilence";
constexpr auto kSilenceTimeout  = "scrolling/silenceTimeout";
constexpr auto kConfirmMultilinePaste = "input/confirmMultilinePaste";
constexpr auto kTrimPastedNewlines    = "input/trimPastedTrailingNewlines";
constexpr auto kGeometry    = "window/qtGeometry";
constexpr auto kWinState    = "window/qtState";
constexpr auto kSidebarWidth = "window/sidebarWidth";

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
    m_appearance.cursorShape =
        m_settings.value(kCursorShape, m_appearance.cursorShape).toInt();
    m_appearance.terminalMargin =
        m_settings.value(kTerminalMargin, m_appearance.terminalMargin).toInt();
    m_appearance.bidi =
        m_settings.value(kBidi, m_appearance.bidi).toBool();
    m_appearance.opacity =
        m_settings.value(kOpacity, m_appearance.opacity).toDouble();

    m_scrolling.scrollback =
        m_settings.value(kScrollback, m_scrolling.scrollback).toInt();
    m_scrolling.scrollbarPosition =
        m_settings.value(kScrollbarPos, m_scrolling.scrollbarPosition).toString();
    m_scrolling.flowControlWarning =
        m_settings.value(kFlowCtrlWarn, m_scrolling.flowControlWarning).toBool();
    m_scrolling.confirmCloseWithRunningProcesses =
        m_settings.value(kConfirmCloseRunning,
                         m_scrolling.confirmCloseWithRunningProcesses).toBool();
    m_scrolling.monitorActivity =
        m_settings.value(kMonitorActivity, m_scrolling.monitorActivity).toBool();
    m_scrolling.monitorSilence =
        m_settings.value(kMonitorSilence, m_scrolling.monitorSilence).toBool();
    m_scrolling.silenceTimeout =
        m_settings.value(kSilenceTimeout, m_scrolling.silenceTimeout).toInt();
    m_scrolling.confirmMultilinePaste =
        m_settings.value(kConfirmMultilinePaste, m_scrolling.confirmMultilinePaste).toBool();
    m_scrolling.trimPastedTrailingNewlines =
        m_settings.value(kTrimPastedNewlines, m_scrolling.trimPastedTrailingNewlines).toBool();

    // Snap to the half-point grid the UI offers.
    m_appearance.fontSize = qRound(m_appearance.fontSize * 2) / 2.0;
    // Keep the cursor shape within the three values the enum defines.
    m_appearance.cursorShape = qBound(0, m_appearance.cursorShape, 2);
    // Opacity below this hides text entirely.
    m_appearance.opacity = qBound(0.3, m_appearance.opacity, 1.0);
}

void Settings::setColorScheme(const QString &v)
{
    if (m_appearance.colorScheme == v) return;
    /*
     * Choosing a scheme by hand supersedes any in-place edit: the live override
     * was only ever a stand-in for one specific scheme, and it is about to be
     * replaced by the user's explicit choice.
     */
    if (!m_liveSchemePath.isEmpty()) {
        ColorSchemeFile::removeLiveCopy(m_liveSchemePath);
        m_liveSchemePath.clear();
        m_liveSchemeRealName.clear();
    }
    m_appearance.colorScheme = v;
    m_settings.setValue(kColorScheme, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setLiveScheme(const QString &realScheme, const QString &livePath)
{
    if (livePath.isEmpty())
        return;

    // Drop a previous live copy; only one override can be in force.
    if (!m_liveSchemePath.isEmpty() && m_liveSchemePath != livePath)
        ColorSchemeFile::removeLiveCopy(m_liveSchemePath);

    m_liveSchemePath = livePath;
    m_liveSchemeRealName = realScheme;
    m_appearance.colorScheme = livePath;

    // Persist the REAL name, never the throwaway file: the live copy does not
    // survive the process, and the next start should resolve back to the user's
    // own scheme.
    m_settings.setValue(kColorScheme, realScheme);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::finaliseLiveSchemes()
{
    if (m_liveSchemePath.isEmpty())
        return;
    const QString real = m_liveSchemeRealName;
    const QString live = m_liveSchemePath;

    m_liveSchemePath.clear();
    m_liveSchemeRealName.clear();
    m_appearance.colorScheme = real;

    ColorSchemeFile::removeLiveCopy(live);
}

void Settings::removeLiveSchemeFiles()
{
    // Any _live_ file in the user dir is a leftover from a previous session
    // (they are deleted as soon as they are superseded), so none should survive.
    const QString dir = ColorSchemeFile::userSchemeDir();
    if (dir.isEmpty())
        return;
    QDir d(dir);
    for (const QString &f : d.entryList({QStringLiteral("_live_*.colorscheme")},
                                        QDir::Files))
        QFile::remove(dir + QLatin1Char('/') + f);
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

void Settings::setCursorShape(int v)
{
    v = qBound(0, v, 2);
    if (m_appearance.cursorShape == v) return;
    m_appearance.cursorShape = v;
    m_settings.setValue(kCursorShape, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setTerminalMargin(int v)
{
    if (m_appearance.terminalMargin == v) return;
    m_appearance.terminalMargin = v;
    m_settings.setValue(kTerminalMargin, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setBidi(bool v)
{
    if (m_appearance.bidi == v) return;
    m_appearance.bidi = v;
    m_settings.setValue(kBidi, v);
    m_settings.sync();
    emit appearanceChanged();
}

void Settings::setOpacity(double v)
{
    v = qBound(0.3, v, 1.0);
    if (qFuzzyCompare(m_appearance.opacity, v)) return;
    m_appearance.opacity = v;
    m_settings.setValue(kOpacity, v);
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

void Settings::setScrollbarPosition(const QString &v)
{
    if (m_scrolling.scrollbarPosition == v) return;
    m_scrolling.scrollbarPosition = v;
    m_settings.setValue(kScrollbarPos, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::setFlowControlWarning(bool v)
{
    if (m_scrolling.flowControlWarning == v) return;
    m_scrolling.flowControlWarning = v;
    m_settings.setValue(kFlowCtrlWarn, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::setConfirmMultilinePaste(bool v)
{
    if (m_scrolling.confirmMultilinePaste == v) return;
    m_scrolling.confirmMultilinePaste = v;
    m_settings.setValue(kConfirmMultilinePaste, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::setTrimPastedTrailingNewlines(bool v)
{
    if (m_scrolling.trimPastedTrailingNewlines == v) return;
    m_scrolling.trimPastedTrailingNewlines = v;
    m_settings.setValue(kTrimPastedNewlines, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::setConfirmCloseWithRunningProcesses(bool v)
{
    if (m_scrolling.confirmCloseWithRunningProcesses == v) return;
    m_scrolling.confirmCloseWithRunningProcesses = v;
    m_settings.setValue(kConfirmCloseRunning, v);
    m_settings.sync();
    emit scrollingChanged();
}

void Settings::setMonitorActivity(bool v)
{
    if (m_scrolling.monitorActivity == v) return;
    m_scrolling.monitorActivity = v;
    m_settings.setValue(kMonitorActivity, v);
    m_settings.sync();
    emit monitoringChanged();
}

void Settings::setMonitorSilence(bool v)
{
    if (m_scrolling.monitorSilence == v) return;
    m_scrolling.monitorSilence = v;
    m_settings.setValue(kMonitorSilence, v);
    m_settings.sync();
    emit monitoringChanged();
}

void Settings::setSilenceTimeout(int v)
{
    if (m_scrolling.silenceTimeout == v) return;
    m_scrolling.silenceTimeout = v;
    m_settings.setValue(kSilenceTimeout, v);
    m_settings.sync();
    emit monitoringChanged();
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

int Settings::sidebarWidth() const
{
    return m_settings.value(kSidebarWidth, 0).toInt();
}

void Settings::setSidebarWidth(int pixels)
{
    m_settings.setValue(kSidebarWidth, pixels);
}
