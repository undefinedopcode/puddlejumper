/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "tmuxworkspace.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QThread>

namespace {

// The window a new session starts with, removed once the real ones exist
// (as smug does), so that every window is made the same way.
const QString kSetupWindow = QStringLiteral("__puddlejumper_setup");

bool run(const QStringList &tmux, const QStringList &args, QString *output, QString *error)
{
    QProcess p;
    p.start(tmux.first(), tmux.mid(1) + args);
    if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            const QString err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
            *error = QStringLiteral("tmux %1: %2").arg(args.value(0),
                                                       err.isEmpty() ? p.errorString() : err);
        }
        return false;
    }
    if (output)
        *output = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    return true;
}

/* An exact session target: "=name" doesn't match other names by prefix. */
QString sessionTarget(const QString &name) { return QLatin1Char('=') + name; }

QJsonObject firstLeaf(QJsonObject node)
{
    while (node.contains(QStringLiteral("panes")))
        node = node.value(QStringLiteral("panes")).toArray().first().toObject();
    return node;
}

QStringList commandsOf(const QJsonObject &leaf)
{
    QStringList out;
    for (const QJsonValue &c : leaf.value(QStringLiteral("commands")).toArray())
        out << c.toString();
    return out;
}

/* -c <dir> for a new pane, and the program for an exec pane. */
QStringList paneArgs(const QJsonObject &leaf, const QString &fallbackDir)
{
    QString dir = leaf.value(QStringLiteral("dir")).toString();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = fallbackDir;
    QStringList args;
    if (!dir.isEmpty())
        args << QStringLiteral("-c") << dir;
    const QStringList commands = commandsOf(leaf);
    if (leaf.value(QStringLiteral("exec")).toBool() && !commands.isEmpty())
        args << commands.join(QStringLiteral("; "));   // tmux runs it with the shell
    return args;
}

/* Lay out the region that pane `pane` occupies as `node` says, then type the
 * commands into its panes. */
bool build(const QStringList &tmux, const WorkspaceSpec &spec, const QJsonObject &node,
           const QString &pane, QString *error)
{
    if (!node.contains(QStringLiteral("panes"))) {
        if (node.value(QStringLiteral("exec")).toBool())
            return true;   // its commands are its program
        for (const QString &command : commandsOf(node)) {
            if (!run(tmux, {QStringLiteral("send-keys"), QStringLiteral("-t"), pane, QStringLiteral("-l"),
                            QStringLiteral("--"), command}, nullptr, error)
                || !run(tmux, {QStringLiteral("send-keys"), QStringLiteral("-t"), pane,
                               QStringLiteral("Enter")}, nullptr, error))
                return false;
            if (spec.sendKeysDelay > 0)
                QThread::msleep(spec.sendKeysDelay);
        }
        return true;
    }

    // Split off each child after the first from what's left, sized as its
    // share of what's left, so the proportions come out as written.
    const QJsonArray children = node.value(QStringLiteral("panes")).toArray();
    const QJsonArray sizesJson = node.value(QStringLiteral("sizes")).toArray();
    QList<int> sizes;
    for (int i = 0; i < children.size(); ++i)
        sizes << qMax(1, sizesJson.at(i).toInt(1));
    const bool across = node.value(QStringLiteral("split")).toString() != QLatin1String("down");

    QStringList panes{pane};
    QString current = pane;
    for (int i = 1; i < children.size(); ++i) {
        int rest = 0;
        int total = sizes.at(i - 1);
        for (int j = i; j < sizes.size(); ++j)
            rest += sizes.at(j);
        total += rest;
        const int percent = qBound(1, qRound(100.0 * rest / total), 99);
        QString created;
        if (!run(tmux, QStringList{QStringLiteral("split-window"), QStringLiteral("-d"), QStringLiteral("-P"),
                                   QStringLiteral("-F"), QStringLiteral("#{pane_id}"),
                                   QStringLiteral("-t"), current,
                                   across ? QStringLiteral("-h") : QStringLiteral("-v"),
                                   QStringLiteral("-l"), QString::number(percent) + QLatin1Char('%')}
                           + paneArgs(firstLeaf(children.at(i).toObject()), spec.root),
                 &created, error))
            return false;
        panes << created;
        current = created;
    }
    for (int i = 0; i < children.size(); ++i) {
        if (!build(tmux, spec, children.at(i).toObject(), panes.at(i), error))
            return false;
    }
    return true;
}

bool makeWindow(const QStringList &tmux, const WorkspaceSpec &spec,
                const WorkspaceSpec::Window &window, bool detached, QString *error)
{
    QStringList args{QStringLiteral("new-window"), QStringLiteral("-P"), QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}"), QStringLiteral("-t"),
                     sessionTarget(spec.name) + QLatin1Char(':')};
    if (detached)
        args << QStringLiteral("-d");
    if (!window.name.isEmpty())
        args << QStringLiteral("-n") << window.name;
    args += paneArgs(firstLeaf(window.layout), spec.root);
    QString pane;
    return run(tmux, args, &pane, error) && build(tmux, spec, window.layout, pane, error);
}

} // namespace

namespace TmuxWorkspace {

bool sessionExists(const QStringList &tmux, const QString &name)
{
    return run(tmux, {QStringLiteral("has-session"), QStringLiteral("-t"), sessionTarget(name)},
               nullptr, nullptr);
}

bool createSession(const QStringList &tmux, const WorkspaceSpec &spec, const QSize &cells,
                   QString *error)
{
    const QSize size = cells.isValid() && !cells.isEmpty() ? cells : QSize(160, 48);
    QStringList args{QStringLiteral("new-session"), QStringLiteral("-d"), QStringLiteral("-s"), spec.name,
                     QStringLiteral("-n"), kSetupWindow,
                     QStringLiteral("-x"), QString::number(size.width()),
                     QStringLiteral("-y"), QString::number(size.height())};
    if (!spec.root.isEmpty() && QDir(spec.root).exists())
        args << QStringLiteral("-c") << spec.root;
    if (!run(tmux, args, nullptr, error))
        return false;

    const auto fail = [&]() {
        run(tmux, {QStringLiteral("kill-session"), QStringLiteral("-t"), sessionTarget(spec.name)},
            nullptr, nullptr);
        return false;
    };

    // Before the windows, so their shells have it.
    for (const QString &pair : spec.environment) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq > 0 && !run(tmux, {QStringLiteral("set-environment"), QStringLiteral("-t"),
                                  sessionTarget(spec.name), pair.left(eq), pair.mid(eq + 1)},
                           nullptr, error))
            return fail();
    }

    QString selected;
    int made = 0;
    for (const WorkspaceSpec::Window &window : spec.windows) {
        if (window.manual)
            continue;
        if (!makeWindow(tmux, spec, window, true, error))
            return fail();
        ++made;
        if (window.selected && !window.name.isEmpty())
            selected = window.name;
    }
    if (made == 0) {
        // Nothing to open: keep the session's first window as a plain shell.
        run(tmux, {QStringLiteral("rename-window"), QStringLiteral("-t"),
                   sessionTarget(spec.name) + QLatin1Char(':') + kSetupWindow, spec.name},
            nullptr, nullptr);
        return true;
    }
    run(tmux, {QStringLiteral("kill-window"), QStringLiteral("-t"),
               sessionTarget(spec.name) + QLatin1Char(':') + kSetupWindow},
        nullptr, nullptr);
    // The selected window, or the first.
    run(tmux, {QStringLiteral("select-window"), QStringLiteral("-t"),
               sessionTarget(spec.name) + QLatin1Char(':')
                   + (selected.isEmpty() ? QStringLiteral("^") : selected)},
        nullptr, nullptr);
    return true;
}

bool addWindow(const QStringList &tmux, const WorkspaceSpec &spec,
               const WorkspaceSpec::Window &window, QString *error)
{
    return makeWindow(tmux, spec, window, false, error);
}

} // namespace TmuxWorkspace
