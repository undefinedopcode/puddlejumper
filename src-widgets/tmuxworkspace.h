/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "workspacefile.h"

#include <QSize>
#include <QString>
#include <QStringList>

/*
 * Builds a workspace file's tmux session, as smug does, with tmux's command
 * line (before puddlejumper attaches to it in control mode): a window per
 * window, panes split to the file's layout, its env set, and its commands
 * typed into the panes.
 */
namespace TmuxWorkspace {

/* Whether tmux has a session called `name`. */
bool sessionExists(const QStringList &tmux, const QString &name);

/*
 * Create the session with the file's windows (not the manual ones), at
 * `cells`: the size puddlejumper's client will be. tmux spreads a later
 * resize evenly over panes rather than in proportion, so a split made at
 * another size wouldn't keep the sizes the file gives.
 */
bool createSession(const QStringList &tmux, const WorkspaceSpec &spec, const QSize &cells,
                   QString *error);

/* Add one window to the running session (a manual one, on request). */
bool addWindow(const QStringList &tmux, const WorkspaceSpec &spec,
               const WorkspaceSpec::Window &window, QString *error);

} // namespace TmuxWorkspace
