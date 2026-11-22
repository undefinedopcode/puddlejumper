/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QtGlobal>

class QMainWindow;
class QToolBar;

/*
 * macOS: put `toolbar` in the title bar, as native apps do, with the window
 * buttons at its left end and no title. The title bar is made transparent and
 * the toolbar sized to it; empty toolbar space drags the window, and a
 * double-click there zooms it.
 *
 * (Qt's own setUnifiedTitleAndToolBarOnMac() only blends the two; in Qt 6 the
 * toolbar stays below a full-height title bar.)
 *
 * Does nothing on other platforms.
 */
#ifdef Q_OS_MACOS
void unifyTitleBar(QMainWindow *window, QToolBar *toolbar);
#else
inline void unifyTitleBar(QMainWindow *, QToolBar *) {}
#endif
