/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QStringList>

/*
 * macOS counts what an app starts, and everything that starts in turn, as
 * part of the app (the app is "responsible" for it). A tmux server started
 * from here would then keep PuddleJumper "running in the background" after
 * it quits, and be stopped along with it, taking the sessions.
 *
 * So a command that may start one goes through the app itself, which turns
 * into the command with that responsibility disclaimed, as Terminal's shells
 * are. Elsewhere the command is run as it is.
 */

/* The command to run in place of `command`. */
QStringList disclaimedCommand(const QStringList &command);

/* First thing in main(): when this process was started for the above,
 * becomes the command, and doesn't return. */
void runDisclaimedIfAsked(int argc, char **argv);
