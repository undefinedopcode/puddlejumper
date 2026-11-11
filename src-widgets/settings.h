/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QSettings>

/*
 * Application settings, backed by QSettings.
 *
 * Deliberately a plain C++ class rather than the QML-facing wrapper in src/:
 * widgets do not need Q_INVOKABLE marshalling, so this reads and writes typed
 * values directly.
 *
 * Key layout is kept identical to the QML build so an existing config keeps
 * working after the port.
 */
class Settings : public QObject
{
    Q_OBJECT

public:
    struct Appearance {
        QString colorScheme = QStringLiteral("Solarized");
        QString fontFamily;                 // resolved at startup if empty
        double  fontSize = 13.0;            // 0.5pt steps
        bool    blinkingCursor = true;
        bool    boldIntense = true;
        bool    italic = true;
        bool    antialias = true;
        /*
         * Draw box-drawing and block characters as vectors rather than taking
         * them from the font. Defaults to true because that is the library
         * default too; exposed as a setting because a font with good
         * box-drawing glyphs may be preferable.
         */
        bool    drawLineChars = true;
        int     lineSpacing = 0;
        QString tabLayout = QStringLiteral("standard"); // |thumbnails
    };

    struct Scrolling {
        int scrollback = 5000;
    };

    explicit Settings(QObject *parent = nullptr);

    const Appearance &appearance() const { return m_appearance; }
    const Scrolling  &scrolling() const { return m_scrolling; }

    void setColorScheme(const QString &v);
    void setFontFamily(const QString &v);
    void setFontSize(double v);
    void setBlinkingCursor(bool v);
    void setBoldIntense(bool v);
    void setItalic(bool v);
    void setAntialias(bool v);
    void setDrawLineChars(bool v);
    void setLineSpacing(int v);
    void setTabLayout(const QString &v);
    void setScrollback(int v);

    void saveWindowGeometry(const QByteArray &geometry, const QByteArray &state);
    QByteArray windowGeometry() const;
    QByteArray windowState() const;

signals:
    void appearanceChanged();
    void scrollingChanged();

private:
    void load();

    QSettings m_settings;
    Appearance m_appearance;
    Scrolling m_scrolling;
};
