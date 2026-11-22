/*
 * puddlejumper — the Windows Subsystem for Linux, as a source of shells (and of
 * tmux, which Windows itself has none of).
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#ifndef WSL_H
#define WSL_H

#include <QStringList>

namespace wsl {

/*
 * The installed distributions' names, the default one first. Empty where
 * there are none, as on every system but Windows.
 */
QStringList distributions();

/* The program and arguments for a shell in @p distribution, at its home. */
QStringList shellCommand(const QString &distribution);

}

#endif // WSL_H
