/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QString>
#include <QVector>

/*
 * One Konsole/qtermwidget colour scheme, held in memory.
 *
 * qtermwidget's own Konsole::ColorScheme is read-only from the outside (its
 * write path is compiled out upstream), and it keeps its table private, so the
 * editor cannot use it to model a scheme. This is a plain value type instead:
 * load a file, hand it to the editor, write it back.
 *
 * There are 20 roles, not 18. The Intense variants cover Foreground and
 * Background too, not just the eight ANSI colours — a scheme that omits those
 * two falls back to black for bold text, which is easy to miss.
 */
struct ColorSchemeFile
{
    /*
     * Role order, fixed by the format. These are the exact section names the
     * parser looks for: getting one wrong silently yields black.
     */
    enum Role {
        Foreground = 0,
        Background,
        Color0, Color1, Color2, Color3, Color4, Color5, Color6, Color7,
        ForegroundIntense, BackgroundIntense,
        Color0Intense, Color1Intense, Color2Intense, Color3Intense,
        Color4Intense, Color5Intense, Color6Intense, Color7Intense,
        RoleCount
    };

    /* File section name for a role, e.g. "Color3Intense". */
    static const char *roleKey(int role);
    /* Human label for a role, e.g. "Bright blue". */
    static QString roleLabel(int role);

    /* The role a terminal uses for its cursor, as a hint to the editor. */
    static constexpr int CursorRole = Foreground;

    QString name;
    QString description;
    QVector<QColor> colors = QVector<QColor>(RoleCount, QColor(Qt::black));

    /*
     * Read a .colorscheme file. Returns false if it does not exist or does not
     * parse. Colours missing from the file keep their previous value, so
     * loading into a pre-seeded object only overwrites what the file defines.
     */
    bool load(const QString &path);

    /*
     * Write a .colorscheme file.
     *
     * Deliberately hand-written rather than QSettings: a comma-separated value
     * is written by QSettings as Color="1,2,3", and qtermwidget's parser only
     * accepts the unquoted form, so every colour would silently load as black.
     */
    bool save(const QString &path) const;

    /*
     * Write a copy of this scheme under a unique throwaway name and return its
     * path, so the terminal can be pointed at it.
     *
     * Needed because qtermwidget keys schemes by file basename and cannot
     * reload one it has already cached: loadColorScheme() deletes the freshly
     * parsed scheme when the name is known, and findColorScheme() keeps
     * returning the stale instance. So an in-place edit of an existing scheme
     * would leave the terminal showing the OLD colours until restart. Applying
     * this freshly-named copy makes the new colours take effect immediately.
     *
     * The caller owns the file and should remove it once it has been loaded.
     */
    QString saveLiveCopy() const;

    /* Remove a file created by saveLiveCopy(). Safe to call on any path. */
    static void removeLiveCopy(const QString &path);

    /* Built-in palette (Konsole's default table), used to seed a new scheme. */
    static ColorSchemeFile defaultScheme();

    /*
     * Directory user-created schemes live in:
     *   ~/.local/share/puddlejumper/color-schemes
     * Created on demand. Registered with qtermwidget at startup.
     */
    static QString userSchemeDir();
    /* Full path a scheme with this name would be saved to. */
    static QString userSchemePath(const QString &name);
    /* Every .colorscheme file in the user dir, by base name. */
    static QStringList userSchemeNames();

    /*
     * Every scheme name the app should offer: qtermwidget's own list plus the
     * user's saved schemes, de-duplicated and sorted.
     *
     * Use this instead of QTermWidget::availableColorSchemes(). That list is
     * cached on first call, so a scheme saved during this run is missing from
     * it -- which silently leaves custom schemes out of any dropdown built from
     * it.
     */
    static QStringList allSchemeNames();

    /*
     * Locate an existing scheme by name anywhere qtermwidget looks (the system
     * dir and the user dir), for use as the seed of a copy. Empty if unknown.
     */
    static QString findSchemeFile(const QString &name);

    /*
     * Is this scheme one of the user's own (i.e. a file in userSchemeDir)?
     *
     * Matters because "edit a copy" exists to protect schemes the user does not
     * own. Their own schemes are edited in place: making them re-save under
     * "X copy" every time they tweak a colour is just friction.
     */
    static bool isUserScheme(const QString &name);

    /*
     * qtermwidget's bundled scheme directory, e.g.
     * /usr/share/qtermwidget6/color-schemes. Empty if not found.
     */
    static QString systemSchemeDir();
};
