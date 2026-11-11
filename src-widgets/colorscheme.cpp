/*
 * superterm — Qt Widgets port.
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

#include <algorithm>

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
                                   return n.startsWith(QLatin1String("_live_"));
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
    return names;
}

QString ColorSchemeFile::systemSchemeDir()
{
    // Matches qtermwidget's COLORSCHEMES_DIR, which is where its bundled
    // schemes live (distro-dependent: /usr/share/qtermwidget6/color-schemes).
    const QStringList candidates = {
        QStringLiteral("/usr/share/qtermwidget6/color-schemes"),
        QStringLiteral("/usr/share/qtermwidget/color-schemes"),
        QStringLiteral("/usr/local/share/qtermwidget6/color-schemes"),
    };
    for (const QString &c : candidates) {
        if (QDir(c).exists())
            return c;
    }
    return QString();
}

QString ColorSchemeFile::findSchemeFile(const QString &name)
{
    if (name.isEmpty())
        return QString();

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
