/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QIcon>
#include <QString>

#include <initializer_list>

/*
 * The first of several freedesktop icon names that the current theme has.
 * Themes disagree on names (Breeze and Adwaita each lack some of the others'),
 * so callers list a few. If none exists the icon is null, and a toolbar button
 * shows its text instead.
 */
inline QIcon themeIcon(std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QString n = QString::fromLatin1(name);
        if (QIcon::hasThemeIcon(n))
            return QIcon::fromTheme(n);
    }
    return QIcon();
}
