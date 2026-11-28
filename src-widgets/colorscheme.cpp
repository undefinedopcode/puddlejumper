/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "colorscheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStandardPaths>
#include <QTextStream>
#include <QCryptographicHash>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>

#include <qtermwidget.h>

namespace {

/*
 * The section name of every role, in Role order. These strings are the file
 * format: the parser matches them exactly, so a typo loads as black with only
 * a console warning.
 */
const char *const kRoleKeys[ColorSchemeFile::RoleCount] = {
    "Foreground",
    "Background",
    "Color0", "Color1", "Color2", "Color3", "Color4", "Color5", "Color6", "Color7",
    "ForegroundIntense",
    "BackgroundIntense",
    "Color0Intense", "Color1Intense", "Color2Intense", "Color3Intense",
    "Color4Intense", "Color5Intense", "Color6Intense", "Color7Intense",
};

// Konsole's own default table, used to seed a brand-new scheme. Kept in step
// with Konsole::ColorScheme::defaultTable.
const QRgb kDefaultTable[ColorSchemeFile::RoleCount] = {
    0x000000, 0xffffff,                                  // fg, bg
    0x000000, 0xb21818, 0x18b218, 0xb26818,              // black..yellow
    0x1818b2, 0xb218b2, 0x18b2b2, 0xb2b2b2,              // blue..white
    0x000000, 0xffffff,                                  // fg/bg intense
    0x686868, 0xff5454, 0x54ff54, 0xffff54,              // intense black..yellow
    0x5454ff, 0xff54ff, 0x54ffff, 0xffffff,              // intense blue..white
};

} // namespace

const char *ColorSchemeFile::roleKey(int role)
{
    if (role < 0 || role >= RoleCount)
        return "";
    return kRoleKeys[role];
}

QString ColorSchemeFile::roleLabel(int role)
{
    // Named for what the user sees, not for the section key: "Bright blue"
    // reads better in the editor than "Color4Intense".
    switch (role) {
    case Foreground:        return QObject::tr("Text");
    case Background:        return QObject::tr("Background");
    case Color0:            return QObject::tr("Black");
    case Color1:            return QObject::tr("Red");
    case Color2:            return QObject::tr("Green");
    case Color3:            return QObject::tr("Yellow");
    case Color4:            return QObject::tr("Blue");
    case Color5:            return QObject::tr("Magenta");
    case Color6:            return QObject::tr("Cyan");
    case Color7:            return QObject::tr("White");
    case ForegroundIntense: return QObject::tr("Text (bold)");
    case BackgroundIntense: return QObject::tr("Background (bold)");
    case Color0Intense:     return QObject::tr("Bright black");
    case Color1Intense:     return QObject::tr("Bright red");
    case Color2Intense:     return QObject::tr("Bright green");
    case Color3Intense:     return QObject::tr("Bright yellow");
    case Color4Intense:     return QObject::tr("Bright blue");
    case Color5Intense:     return QObject::tr("Bright magenta");
    case Color6Intense:     return QObject::tr("Bright cyan");
    case Color7Intense:     return QObject::tr("Bright white");
    }
    return QString();
}

QString ColorSchemeFile::saveLiveCopy() const
{
    /*
     * The name must be unique for the life of the process, because
     * ColorSchemeManager never forgets a basename it has loaded. Timestamp plus
     * a counter covers repeated saves within the same run.
     */
    static int counter = 0;
    const QString base = QStringLiteral("_live_%1_%2")
                             .arg(QDateTime::currentMSecsSinceEpoch())
                             .arg(++counter);
    const QString path = userSchemePath(base);
    if (path.isEmpty())
        return QString();

    ColorSchemeFile copy = *this;
    copy.name = base;
    copy.description = base;
    if (!copy.save(path))
        return QString();
    return path;
}

void ColorSchemeFile::removeLiveCopy(const QString &path)
{
    // Only ever delete something matching the throwaway pattern, so a bad
    // pointer cannot take a real scheme with it.
    if (path.isEmpty())
        return;
    const QString base = QFileInfo(path).baseName();
    if (!base.startsWith(QLatin1String("_live_")))
        return;
    QFile::remove(path);
}

ColorSchemeFile ColorSchemeFile::defaultScheme()
{
    ColorSchemeFile s;
    s.name = QObject::tr("Custom");
    s.description = s.name;
    for (int i = 0; i < RoleCount; ++i)
        s.colors[i] = QColor(QRgb(kDefaultTable[i]));
    return s;
}

bool ColorSchemeFile::load(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    /*
     * Parsed by hand rather than with QSettings for the same reason save()
     * writes by hand: QSettings reinterprets a comma value as a QStringList,
     * which makes "1,2,3" awkward to read back, and it collapses sections in
     * ways this simple format does not need.
     */
    QTextStream in(&f);
    QString section;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();

        if (section.compare(QLatin1String("General"), Qt::CaseInsensitive) == 0) {
            if (key.compare(QLatin1String("Description"), Qt::CaseInsensitive) == 0)
                description = value;
            continue;
        }

        if (key.compare(QLatin1String("Color"), Qt::CaseInsensitive) != 0)
            continue;

        for (int i = 0; i < RoleCount; ++i) {
            if (section.compare(QLatin1String(kRoleKeys[i]),
                                Qt::CaseInsensitive) != 0)
                continue;
            // Accept both "r,g,b" and a plain #rrggbb, since hand-edited files
            // in the wild use either.
            QColor c;
            const QStringList parts =
                value.split(QLatin1Char(','), Qt::SkipEmptyParts);
            if (parts.size() == 3) {
                c = QColor(parts[0].trimmed().toInt(),
                           parts[1].trimmed().toInt(),
                           parts[2].trimmed().toInt());
            } else {
                c = QColor(value);
            }
            if (c.isValid())
                colors[i] = c;
            break;
        }
    }

    // The name is the filename, not anything inside the file: that is what
    // qtermwidget uses, so the editor must agree with it.
    name = QFileInfo(path).baseName();
    if (description.isEmpty())
        description = name;
    return true;
}

bool ColorSchemeFile::save(const QString &path) const
{
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir))
        return false;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    QTextStream out(&f);
    out << "[General]\n"
        << "Description=" << (description.isEmpty() ? name : description) << "\n"
        << "Opacity=1\n"
        << "Wallpaper=\n\n";

    for (int i = 0; i < RoleCount; ++i) {
        const QColor c = colors.at(i);
        out << "[" << roleKey(i) << "]\n"
            // Unquoted, no spaces — the parser rejects anything else.
            << "Color=" << c.red() << ',' << c.green() << ',' << c.blue() << '\n'
            << "Transparent=false\n\n";
    }
    out.flush();
    return f.error() == QFile::NoError;
}

QString ColorSchemeFile::userSchemeDir()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return QString();
    return base + QStringLiteral("/color-schemes");
}

QString ColorSchemeFile::userSchemePath(const QString &name)
{
    const QString dir = userSchemeDir();
    if (dir.isEmpty() || name.isEmpty())
        return QString();
    return dir + QLatin1Char('/') + name + QStringLiteral(".colorscheme");
}

QStringList ColorSchemeFile::userSchemeNames()
{
    QDir d(userSchemeDir());
    if (!d.exists())
        return {};
    const QStringList files =
        d.entryList({QStringLiteral("*.colorscheme")}, QDir::Files, QDir::Name);
    QStringList names;
    names.reserve(files.size());
    for (const QString &f : files) {
        const QString base = QFileInfo(f).baseName();
        // Skip live stand-ins: they are an implementation detail of in-place
        // editing and must never appear in a scheme list.
        if (base.startsWith(QLatin1String("_live_")))
            continue;
        names << base;
    }
    return names;
}

QStringList ColorSchemeFile::allSchemeNames()
{
    /*
     * availableColorSchemes() is cached after its first call, so it never
     * learns about schemes written during this run. Merge in the user dir
     * every time, so custom schemes show up immediately in every dropdown.
     */
    QStringList names = QTermWidget::availableColorSchemes();
    /*
     * Filter the library's list too: setColorScheme() on a live stand-in causes
     * ColorSchemeManager to cache it, so it turns up here even though
     * userSchemeNames() skips it. A live stand-in is an implementation detail
     * and must never appear in a scheme list.
     */
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const QString &n) {
                                   return n.startsWith(QLatin1String("_live_"))
                                          || n.startsWith(QLatin1String("_system_"));
                               }),
                names.end());

    for (const QString &extra : userSchemeNames()) {
        bool known = false;
        for (const QString &n : names) {
            if (n.compare(extra, Qt::CaseInsensitive) == 0) {
                known = true;
                break;
            }
        }
        if (!known)
            names << extra;
    }
    names.sort(Qt::CaseInsensitive);
    names.removeAll(systemName());
    names.prepend(systemName());   // first: the one that follows the desktop
    return names;
}

QString ColorSchemeFile::systemSchemeDir()
{
    // Matches the vendored qtermwidget's COLORSCHEMES_DIR: its bundled schemes
    // are compiled in as Qt resources (third_party/qtermwidget/CMakeLists.txt).
    return QStringLiteral(":/qtermwidget/color-schemes");
}

QString ColorSchemeFile::findSchemeFile(const QString &name)
{
    if (name.isEmpty())
        return QString();
    if (name == systemName())
        return systemSchemeFile();

    const QString fileName = name + QStringLiteral(".colorscheme");

    // User schemes first, so editing a copy of your own scheme picks your
    // version rather than a system one with the same name.
    const QString userDir = userSchemeDir();
    if (!userDir.isEmpty()) {
        const QString p = userDir + QLatin1Char('/') + fileName;
        if (QFile::exists(p))
            return p;
    }

    const QString sysDir = systemSchemeDir();
    if (!sysDir.isEmpty()) {
        const QString p = sysDir + QLatin1Char('/') + fileName;
        if (QFile::exists(p))
            return p;
    }
    return QString();
}

bool ColorSchemeFile::isUserScheme(const QString &name)
{
    if (name.isEmpty())
        return false;
    const QString dir = userSchemeDir();
    if (dir.isEmpty())
        return false;
    // A name may arrive as a full path (that is what Settings stores after an
    // edit); treat that as the user's own only if it really lives in their dir.
    if (name.contains(QLatin1Char('/'))) {
        // QDir::absolutePath(), not QFileInfo(dir).absolutePath(): on a
        // directory the latter strips the last path component, so the
        // comparison would never match.
        const QString nameDir = QFileInfo(name).absolutePath();
        const QString ownDir = QDir(dir).absolutePath();
        return nameDir == ownDir;
    }
    return QFile::exists(dir + QLatin1Char('/') + name
                         + QStringLiteral(".colorscheme"));
}

// ---- System ------------------------------------------------------------------

namespace {

// OKLab (Björn Ottosson's), for hues and lightness as they look.
struct Lab { double L, a, b; };

double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double fromLinear(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

Lab toLab(const QColor &c)
{
    const double r = toLinear(c.redF()), g = toLinear(c.greenF()), b = toLinear(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

// Linear sRGB, which may be out of gamut.
void labToLinear(const Lab &c, double rgb[3])
{
    const double l = std::pow(c.L + 0.3963377774 * c.a + 0.2158037573 * c.b, 3);
    const double m = std::pow(c.L - 0.1055613458 * c.a - 0.0638541728 * c.b, 3);
    const double s = std::pow(c.L - 0.0894841775 * c.a - 1.2914855480 * c.b, 3);
    rgb[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    rgb[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    rgb[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

// Lightness, chroma and hue (degrees) as a colour, the chroma reduced until
// it's one sRGB can show.
QColor lch(double L, double C, double h)
{
    L = std::clamp(L, 0.0, 1.0);
    const double rad = h * M_PI / 180;
    double rgb[3];
    for (int i = 0; i < 40; ++i, C *= 0.92) {
        labToLinear({L, C * std::cos(rad), C * std::sin(rad)}, rgb);
        if (std::all_of(rgb, rgb + 3, [](double v) { return v >= -1e-4 && v <= 1 + 1e-4; }))
            break;
    }
    return QColor::fromRgbF(float(std::clamp(fromLinear(std::clamp(rgb[0], 0.0, 1.0)), 0.0, 1.0)),
                            float(std::clamp(fromLinear(std::clamp(rgb[1], 0.0, 1.0)), 0.0, 1.0)),
                            float(std::clamp(fromLinear(std::clamp(rgb[2], 0.0, 1.0)), 0.0, 1.0)));
}

double luminance(const QColor &c)
{
    return 0.2126 * toLinear(c.redF()) + 0.7152 * toLinear(c.greenF()) + 0.0722 * toLinear(c.blueF());
}

double hueOf(const Lab &c) { return std::fmod(std::atan2(c.b, c.a) * 180 / M_PI + 360, 360); }
double chromaOf(const Lab &c) { return std::hypot(c.a, c.b); }
double hueDistance(double a, double b) { const double d = std::fabs(a - b); return std::min(d, 360 - d); }

// Starting at lightness L, moved away from the background until it reads at
// `ratio` (or as far as lightness goes).
QColor readable(double L, double C, double h, const QColor &bg, bool dark, double ratio)
{
    QColor c = lch(L, C, h);
    for (int i = 0; i < 100 && ColorSchemeFile::contrast(c, bg) < ratio; ++i) {
        L += dark ? 0.01 : -0.01;
        if (L < 0 || L > 1)
            break;
        c = lch(L, C, h);
    }
    return c;
}

} // namespace

double ColorSchemeFile::contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QString ColorSchemeFile::systemName()
{
    return QStringLiteral("System");
}

ColorSchemeFile ColorSchemeFile::systemScheme(const QPalette &palette)
{
    const QColor bg = palette.color(QPalette::Active, QPalette::Base);
    QColor fg = palette.color(QPalette::Active, QPalette::Text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    const QColor accent = palette.color(QPalette::Active, QPalette::Accent);
#else
    const QColor accent = palette.color(QPalette::Active, QPalette::Highlight);
#endif
    const Lab back = toLab(bg), front = toLab(fg), acc = toLab(accent);
    const bool dark = back.L < front.L;
    // The text as the desktop has it, unless it's hard to read.
    if (contrast(fg, bg) < 7.0)
        fg = readable(front.L, chromaOf(front), hueOf(front), bg, dark, 7.0);

    ColorSchemeFile s;
    s.name = systemName();
    s.description = QObject::tr("System (follows the desktop's colours)");
    s.colors[Background] = bg;
    s.colors[BackgroundIntense] = bg;
    s.colors[Foreground] = fg;
    s.colors[ForegroundIntense] = readable(toLab(fg).L + (dark ? 0.06 : -0.06), chromaOf(front), hueOf(front),
                                           bg, dark, contrast(fg, bg));

    // The six colours by their hue (OKLCH degrees). The nearest one leans
    // towards the accent's hue (a little, so the six stay apart).
    struct Hue { int normal, bright; double h; };
    Hue hues[] = {
        {Color1, Color1Intense, 29},   // red
        {Color2, Color2Intense, 142},  // green
        {Color3, Color3Intense, 95},   // yellow
        {Color4, Color4Intense, 260},  // blue
        {Color5, Color5Intense, 328},  // magenta
        {Color6, Color6Intense, 195},  // cyan
    };
    if (chromaOf(acc) > 0.06) {
        Hue *nearest = std::min_element(std::begin(hues), std::end(hues), [&](const Hue &x, const Hue &y) {
            return hueDistance(x.h, hueOf(acc)) < hueDistance(y.h, hueOf(acc));
        });
        double lean = std::fmod(hueOf(acc) - nearest->h + 540, 360) - 180;   // -180..180
        if (std::fabs(lean) < 30)
            nearest->h += std::clamp(lean, -15.0, 15.0);
    }
    for (const Hue &hue : hues) {
        s.colors[hue.normal] = readable(dark ? 0.72 : 0.52, 0.14, hue.h, bg, dark, 4.5);
        s.colors[hue.bright] = readable(dark ? 0.82 : 0.60, 0.16, hue.h, bg, dark, 3.0);
    }

    // Black and white, a little of the background's hue in them. On a dark
    // background black is a shade above it (TUIs fill bars with it) and
    // bright black the grey of comments; white is the text. On a light one,
    // the other way round.
    const double tint = std::min(0.02, chromaOf(back)), th = hueOf(back);
    if (dark) {
        s.colors[Color0] = readable(back.L + 0.10, tint, th, bg, true, 1.4);
        s.colors[Color0Intense] = readable(back.L + 0.30, tint, th, bg, true, 3.0);
        s.colors[Color7] = readable(0.82, tint, th, bg, true, 7.0);
        s.colors[Color7Intense] = readable(0.95, tint, th, bg, true, 7.0);
    } else {
        s.colors[Color0] = readable(0.25, tint, th, bg, false, 7.0);
        s.colors[Color0Intense] = readable(back.L - 0.35, tint, th, bg, false, 3.0);
        s.colors[Color7] = readable(back.L - 0.22, tint, th, bg, false, 3.0);
        s.colors[Color7Intense] = readable(back.L - 0.10, tint, th, bg, false, 1.4);
    }
    return s;
}

QString ColorSchemeFile::systemSchemeFile()
{
    const ColorSchemeFile s = systemScheme(QGuiApplication::palette());
    QByteArray key;
    for (const QColor &c : s.colors)
        key += c.name().toLatin1();
    const QString base = QStringLiteral("_system_") + QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(12));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + QStringLiteral("/color-schemes");
    if (!QDir().mkpath(dir))
        return QString();
    const QString path = dir + QLatin1Char('/') + base + QStringLiteral(".colorscheme");
    if (!QFile::exists(path)) {
        // Only the one for the colours now: earlier ones are loaded already
        // (or never will be again).
        for (const QFileInfo &old : QDir(dir).entryInfoList({QStringLiteral("_system_*.colorscheme")}, QDir::Files))
            QFile::remove(old.absoluteFilePath());
        ColorSchemeFile named = s;
        named.name = base;
        if (!named.save(path))
            return QString();
    }
    return path;
}
