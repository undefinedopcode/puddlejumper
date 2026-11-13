/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QString>
#include <QStringList>

/*
 * What a process on this machine is running, for tab titles and for saving a
 * workspace. Read from /proc on Linux and from libproc/sysctl on macOS; on
 * other platforms each returns nothing, and callers carry on without it.
 */
namespace procinfo {

// The process's short name ("vim", "ssh"). Linux truncates it to 15
// characters, macOS to 32.
QString name(int pid);

// Its command line, argv[0] first.
QStringList arguments(int pid);

// The foreground process group of the terminal the process is attached to,
// or -1 if it has none or it can't be read.
int terminalForegroundGroup(int pid);

} // namespace procinfo
