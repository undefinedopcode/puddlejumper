/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QIcon>

/*
 * One of the Lucide icons bundled under icons/lucide/ (https://lucide.dev),
 * by its Lucide name ("settings", "zoom-in"). The same set is used on every
 * platform rather than the desktop's icon theme: macOS has none, and themes
 * disagree on which freedesktop names they carry.
 *
 * Lucide draws in currentColor, so the icon is painted in the application
 * palette's text colour (disabled and selected variants included) each time
 * it is drawn, and follows a switch between light and dark.
 *
 * An unknown name gives a null icon, and a toolbar button shows its text.
 */
QIcon appIcon(const char *lucideName);
/* The same, always in `colour` (dimmed when disabled): for icons drawn over a
 * terminal's background rather than the window's. */
QIcon appIcon(const char *lucideName, const QColor &colour);
