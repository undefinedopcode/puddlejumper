/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "appicon.h"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace {

QString resourcePath(const QString &name)
{
    // A name with a slash is another bundled set's ("connections/rex-tab-icon").
    if (name.contains(QLatin1Char('/')))
        return QStringLiteral(":/%1.svg").arg(name);
    return QStringLiteral(":/lucide/%1.svg").arg(name);
}

// The SVG source, read from resources once per icon.
QByteArray svgSource(const QString &name)
{
    static QHash<QString, QByteArray> sources;
    auto it = sources.find(name);
    if (it == sources.end()) {
        QFile f(resourcePath(name));
        it = sources.insert(name, f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
    }
    return *it;
}

QColor colourFor(QIcon::Mode mode)
{
    const QPalette pal = QGuiApplication::palette();
    switch (mode) {
    case QIcon::Disabled: return pal.color(QPalette::Disabled, QPalette::WindowText);
    case QIcon::Selected: return pal.color(QPalette::Active, QPalette::HighlightedText);
    default:              return pal.color(QPalette::Active, QPalette::WindowText);
    }
}

class LucideIconEngine : public QIconEngine
{
public:
    explicit LucideIconEngine(const QString &name, const QColor &fixed = {}) : m_name(name), m_fixed(fixed) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        // The colour goes into the SVG without its alpha, which is applied as
        // opacity instead: macOS's palette is translucent (text at 85%,
        // disabled text at 25%), and that is all that tells disabled apart.
        const QColor colour = colour_(mode);
        QByteArray svg = svgSource(m_name);
        svg.replace("currentColor", colour.name(QColor::HexRgb).toLatin1());
        QSvgRenderer renderer(svg);
        // Lucide's viewBox is square; keep it so in a non-square rect.
        const int side = qMin(rect.width(), rect.height());
        QRect square(0, 0, side, side);
        square.moveCenter(rect.center());
        painter->save();
        painter->setOpacity(painter->opacity() * colour.alphaF());
        renderer.render(painter, square);
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        // Widgets ask for the pixmap on every repaint; rendering the SVG each
        // time is wasteful, so cache per size, scale and colour.
        const QString key = QStringLiteral("puddlejumper-lucide/%1/%2x%3@%4/%5")
                                .arg(m_name).arg(size.width()).arg(size.height())
                                .arg(scale).arg(colour_(mode).rgba());
        QPixmap pm;
        if (QPixmapCache::find(key, &pm))
            return pm;
        pm = QPixmap((QSizeF(size) * scale).toSize());
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(scale, scale);
            paint(&p, QRect(QPoint(), size), mode, state);
        }
        pm.setDevicePixelRatio(scale);
        QPixmapCache::insert(key, pm);
        return pm;
    }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
    {
        const int side = qMin(size.width(), size.height());
        return {side, side};
    }

    QIconEngine *clone() const override { return new LucideIconEngine(m_name, m_fixed); }
    QString key() const override { return QStringLiteral("puddlejumper-lucide"); }
    QString iconName() override { return m_name; }

private:
    // A fixed colour (dimmed when disabled), or the palette's.
    QColor colour_(QIcon::Mode mode) const
    {
        if (!m_fixed.isValid())
            return colourFor(mode);
        QColor c = m_fixed;
        if (mode == QIcon::Disabled)
            c.setAlphaF(c.alphaF() * 0.35);
        return c;
    }

    QString m_name;
    QColor m_fixed;
};

} // namespace

QIcon appIcon(const char *lucideName)
{
    const QString name = QString::fromLatin1(lucideName);
    if (svgSource(name).isEmpty())
        return QIcon();
    return QIcon(new LucideIconEngine(name));
}

QIcon appIcon(const char *lucideName, const QColor &colour)
{
    const QString name = QString::fromLatin1(lucideName);
    if (svgSource(name).isEmpty())
        return QIcon();
    return QIcon(new LucideIconEngine(name, colour));
}
