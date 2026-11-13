/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

/*
 * A workspace file: smug's YAML format (https://github.com/ivaaaan/smug),
 * so existing smug files open as they are, plus a few keys of superterm's
 * own that smug ignores:
 *
 *   session: name              root: ~/src/project       env: {KEY: value}
 *   before_start: [commands]   stop: [commands]          sendkeys_timeout: ms
 *   colour: "#4a8fe0"          profile: <profile name>   (superterm)
 *   backend: tmux | local                                (superterm)
 *   windows:
 *     - name: code             root: relative/or/abs     layout: main-vertical
 *       commands: [...]        panes: [{root, commands, type}]
 *       manual: true           selected: true
 *       profile, exec: true                              (superterm)
 *       split: right|down      sizes: [60, 40]           (superterm: nested
 *       panes: [{split, sizes, panes} | {root, commands}]    layouts)
 *
 * $name and ${name} are expanded over the whole file before it is read, as
 * smug does: from the values given, then the environment.
 */
struct WorkspaceSpec
{
    struct Window
    {
        QString name;
        QString root;         // absolute
        /*
         * As TerminalTab::restoreLayout() takes it. Leaves carry "dir",
         * "commands" (typed into the shell), "exec" (run instead of a shell)
         * and "profile" (a name).
         */
        QJsonObject layout;
        bool manual = false;   // listed, started on request
        bool selected = false; // the tab shown first
    };

    QString path;
    QString name;
    QString root;              // absolute; empty for the profile's default
    QColor colour;
    QStringList environment;   // NAME=value, for every pane
    QStringList beforeStart;   // run in root before opening; a failure stops it
    QStringList stop;          // run in root when the workspace is closed
    int sendKeysDelay = 0;     // ms between commands typed into a pane
    /*
     * Run as a tmux session (named `name`), as smug does, so it outlives
     * superterm; otherwise as superterm's own shells. `backend:` says which;
     * without it, smug's files (in its directory, or .smug.yml) use tmux.
     */
    bool tmux = false;
    QList<Window> windows;
};

namespace WorkspaceFile {

/* Where named workspace files are found: superterm's directory, then smug's. */
QStringList directories();
/* The files there, as (name, path); a name in superterm's directory wins. */
QList<QPair<QString, QString>> available();
/* Whether `path` is in one of directories(): the user's own, not a project's. */
bool isUsersOwn(const QString &path);

/* A project's workspace file in `dir`: .superterm.yml, else .smug.yml
 * (or .yaml); empty if it has none. */
QString projectFileIn(const QString &dir);
/* The nearest one in `dir` or a folder above it, stopping short of the home
 * folder (and the root); empty if none. */
QString findProjectFile(const QString &dir);

/*
 * Variables `text` uses that neither `values` nor the environment define.
 * Those the file sets in its own env: are left alone: the panes have them,
 * so their shells expand them.
 */
QStringList missingVariables(const QString &text, const QHash<QString, QString> &values);
/* Expand $name and ${name} from `values`, then the environment (else empty),
 * except the names the file's env: sets. */
QString expand(const QString &text, const QHash<QString, QString> &values);

/* Read an expanded file. False, with a message in `error`, if it isn't valid. */
bool parse(const QString &text, const QString &path, WorkspaceSpec &out, QString *error);

/* Every command the file would run, for asking before trusting it. */
QStringList commands(const WorkspaceSpec &spec);

/* A window of an open workspace, to be written back as a file. */
struct SavedWindow
{
    QString name;
    QJsonObject layout;   // TerminalTab::saveLayout(); leaves: "dir", "commands"
};

/*
 * A workspace file for these windows, in smug's format where it can be.
 * `basedOn`, the text of the file the workspace was opened from, if any,
 * lends what an open workspace doesn't show: its env, before_start, stop and
 * other settings, and its manual windows that aren't open (all unexpanded).
 * `tmux` writes backend: tmux, unless the original says otherwise.
 */
QString write(const QString &name, const QString &root, const QColor &colour,
              const QList<SavedWindow> &windows, const QString &basedOn = QString(),
              bool tmux = false);

} // namespace WorkspaceFile
