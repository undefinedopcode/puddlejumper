/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "workspacefile.h"

#include "terminaltab.h"   // PaneLayout
#include "tmuxcontrol.h"   // parseLayout()

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <yaml-cpp/yaml.h>

#include <cmath>

namespace {

const QRegularExpression &variablePattern()
{
    static const QRegularExpression re(
        QStringLiteral("\\$(?:\\{([A-Za-z_][A-Za-z0-9_]*)\\}|([A-Za-z_][A-Za-z0-9_]*))"));
    return re;
}

/*
 * Load YAML that may not be valid until its variables are expanded (as in
 * "[echo ${name}]"): try it as it is, then with every variable replaced by a
 * placeholder. Only its shape is wanted. Null if neither loads.
 */
YAML::Node loadUnexpanded(const QString &text)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        QString t = text;
        if (attempt == 1)
            t.replace(variablePattern(), QStringLiteral("x"));
        try {
            return YAML::Load(t.toStdString());
        } catch (const YAML::Exception &) {
        }
    }
    return YAML::Node();
}

/* The names a file's env: sets, read before anything is expanded. */
QStringList ownEnvironment(const QString &text)
{
    QStringList out;
    const YAML::Node doc = loadUnexpanded(text);
    try {
        if (doc.IsMap() && doc["env"] && doc["env"].IsMap()) {
            for (const auto &kv : doc["env"])
                out << QString::fromStdString(kv.first.as<std::string>());
        }
    } catch (const YAML::Exception &) {
    }
    return out;
}

/*
 * A file's top-level entries as text, (key, lines) in order, so they can be
 * written back exactly as they were: variables, comments and all.
 */
QList<QPair<QString, QString>> topLevelBlocks(const QString &text)
{
    static const QRegularExpression keyLine(QStringLiteral("^([A-Za-z_][A-Za-z0-9_-]*)\\s*:"));
    QList<QPair<QString, QString>> out;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const auto m = keyLine.match(line);
        if (m.hasMatch())
            out << qMakePair(m.captured(1), QString());
        if (!out.isEmpty())
            out.last().second += line + QLatin1Char('\n');
    }
    return out;
}

/* The items of a top-level sequence block ("windows:\n  - ...\n  - ..."),
 * each re-indented to start at column 0. */
QStringList sequenceItems(const QString &block)
{
    QStringList lines = block.split(QLatin1Char('\n'));
    lines.removeFirst();   // the key
    static const QRegularExpression itemLine(QStringLiteral("^(\\s*)- "));
    int indent = -1;
    QStringList items;
    for (const QString &line : std::as_const(lines)) {
        const auto m = itemLine.match(line);
        if (m.hasMatch() && (indent < 0 || m.captured(1).size() == indent)) {
            indent = m.captured(1).size();
            items << QString();
        }
        if (!items.isEmpty()) {
            int strip = 0;
            while (strip < indent && strip < line.size() && line.at(strip) == QLatin1Char(' '))
                ++strip;
            items.last() += line.mid(strip) + QLatin1Char('\n');
        }
    }
    return items;
}

QString indented(const QString &text, int spaces)
{
    const QString pad(spaces, QLatin1Char(' '));
    QString out;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.isEmpty())
            out += pad + line;
        out += QLatin1Char('\n');
    }
    while (out.endsWith(QStringLiteral("\n\n")))
        out.chop(1);
    return out;
}

QString expandHome(const QString &path)
{
    if (path == QLatin1String("~"))
        return QDir::homePath();
    if (path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

/* `path` against `base`, as smug does: absolute stays, relative joins. */
QString resolve(const QString &base, const QString &path)
{
    const QString p = expandHome(path);
    if (!p.isEmpty() && QDir::isAbsolutePath(p))
        return QDir::cleanPath(p);
    const QString b = base.isEmpty() ? QDir::homePath() : base;
    return QDir::cleanPath(p.isEmpty() ? b : b + QLatin1Char('/') + p);
}

QString str(const YAML::Node &node, const char *key)
{
    const YAML::Node v = node[key];
    return v && v.IsScalar() ? QString::fromStdString(v.as<std::string>()) : QString();
}

bool flag(const YAML::Node &node, const char *key)
{
    const YAML::Node v = node[key];
    return v && v.IsScalar() && v.as<bool>(false);
}

/* A list of commands; a single string counts as one. */
QStringList list(const YAML::Node &node, const char *key)
{
    QStringList out;
    const YAML::Node v = node[key];
    if (!v)
        return out;
    if (v.IsScalar()) {
        out << QString::fromStdString(v.as<std::string>());
    } else if (v.IsSequence()) {
        for (const YAML::Node &item : v) {
            if (item.IsScalar())
                out << QString::fromStdString(item.as<std::string>());
        }
    }
    return out;
}

QJsonObject leaf(const QString &dir, const QStringList &commands, const QString &profile, bool exec)
{
    QJsonObject o{{QStringLiteral("dir"), dir}};
    if (!commands.isEmpty())
        o.insert(QStringLiteral("commands"), QJsonArray::fromStringList(commands));
    if (!profile.isEmpty())
        o.insert(QStringLiteral("profile"), profile);
    if (exec)
        o.insert(QStringLiteral("exec"), true);
    return o;
}

QJsonObject split(const QString &direction, const QList<QJsonObject> &parts,
                  const QList<int> &sizes = {})
{
    if (parts.size() == 1)
        return parts.first();
    QJsonArray panes;
    QJsonArray sz;
    for (int i = 0; i < parts.size(); ++i) {
        panes.append(parts.at(i));
        sz.append(sizes.value(i, 1));
    }
    return {
        {QStringLiteral("split"), direction},
        {QStringLiteral("sizes"), sz},
        {QStringLiteral("panes"), panes},
    };
}

/* A tmux layout string's tree, its leaves taken in order from `leaves`. */
QJsonObject fromPaneLayout(const PaneLayout &node, const QList<QJsonObject> &leaves, int &next)
{
    if (node.isLeaf())
        return leaves.value(next++, leaves.value(0));
    QList<QJsonObject> parts;
    QList<int> sizes;
    for (const PaneLayout &child : node.children) {
        parts << fromPaneLayout(child, leaves, next);
        sizes << qMax(1, node.orientation == Qt::Horizontal ? child.columns : child.lines);
    }
    return split(node.orientation == Qt::Horizontal ? QStringLiteral("right")
                                                    : QStringLiteral("down"),
                 parts, sizes);
}

/* tmux's layouts, by name or as a layout string, for panes in order. */
QJsonObject arrange(const QString &layout, const QList<QJsonObject> &leaves)
{
    const int n = leaves.size();
    if (n == 1)
        return leaves.first();
    const QList<QJsonObject> rest = leaves.mid(1);
    const auto restAs = [&](const QString &direction) { return split(direction, rest); };

    if (layout == QLatin1String("even-vertical"))
        return split(QStringLiteral("down"), leaves);
    if (layout == QLatin1String("main-vertical"))
        return split(QStringLiteral("right"), {leaves.first(), restAs(QStringLiteral("down"))});
    if (layout == QLatin1String("main-vertical-mirrored"))
        return split(QStringLiteral("right"), {restAs(QStringLiteral("down")), leaves.first()});
    if (layout == QLatin1String("main-horizontal"))
        return split(QStringLiteral("down"), {leaves.first(), restAs(QStringLiteral("right"))});
    if (layout == QLatin1String("main-horizontal-mirrored"))
        return split(QStringLiteral("down"), {restAs(QStringLiteral("right")), leaves.first()});
    if (layout == QLatin1String("tiled")) {
        // Rows of equal panes, filled in order, as tmux does.
        const int columns = int(std::ceil(std::sqrt(double(n))));
        QList<QJsonObject> rows;
        for (int i = 0; i < n; i += columns)
            rows << split(QStringLiteral("right"), leaves.mid(i, columns));
        return split(QStringLiteral("down"), rows);
    }
    if (layout.contains(QLatin1Char(','))) {
        // A layout string, as `tmux list-windows` prints it.
        PaneLayout parsed;
        if (TmuxControl::parseLayout(layout.toUtf8(), parsed)) {
            int next = 0;
            return fromPaneLayout(parsed, leaves, next);
        }
    }
    // even-horizontal, smug's default; and anything not understood.
    return split(QStringLiteral("right"), leaves);
}

/* superterm's nested form: a node with "split" and "panes", or a pane. */
QJsonObject nested(const YAML::Node &node, const QString &base, const QString &profile,
                   QStringList firstCommands)
{
    const QString direction = str(node, "split");
    if (direction.isEmpty() || !node["panes"] || !node["panes"].IsSequence()) {
        const QString paneProfile = str(node, "profile");
        return leaf(resolve(base, str(node, "root")), firstCommands + list(node, "commands"),
                    paneProfile.isEmpty() ? profile : paneProfile, flag(node, "exec"));
    }
    QList<QJsonObject> parts;
    for (const YAML::Node &child : node["panes"]) {
        parts << nested(child, base, profile, firstCommands);
        firstCommands.clear();   // the window's own commands go to its first pane
    }
    QList<int> sizes;
    if (node["sizes"] && node["sizes"].IsSequence()) {
        for (const YAML::Node &s : node["sizes"])
            sizes << qMax(1, s.as<int>(1));
    }
    return split(direction == QLatin1String("down") ? QStringLiteral("down")
                                                     : QStringLiteral("right"),
                 parts, sizes);
}

void collectCommands(const QJsonObject &node, QStringList &out)
{
    for (const QJsonValue &c : node.value(QStringLiteral("commands")).toArray())
        out << c.toString();
    for (const QJsonValue &child : node.value(QStringLiteral("panes")).toArray())
        collectCommands(child.toObject(), out);
}

// ---- writing ----------------------------------------------------------------

/* `path` relative to `base` if under it; else from ~ if under home. */
QString relativeTo(const QString &base, const QString &path)
{
    if (path.isEmpty())
        return path;
    if (!base.isEmpty() && path == base)
        return QString();
    if (!base.isEmpty() && path.startsWith(base + QLatin1Char('/')))
        return path.mid(base.size() + 1);
    const QString home = QDir::homePath();
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

void emitCommands(YAML::Emitter &out, const QJsonObject &leafNode)
{
    const QJsonArray commands = leafNode.value(QStringLiteral("commands")).toArray();
    if (commands.isEmpty())
        return;
    out << YAML::Key << "commands" << YAML::Value << YAML::BeginSeq;
    for (const QJsonValue &c : commands)
        out << c.toString().toStdString();
    out << YAML::EndSeq;
}

void emitRoot(YAML::Emitter &out, const QString &base, const QJsonObject &leafNode)
{
    const QString root = relativeTo(base, leafNode.value(QStringLiteral("dir")).toString());
    if (!root.isEmpty())
        out << YAML::Key << "root" << YAML::Value << root.toStdString();
}

void emitNested(YAML::Emitter &out, const QString &base, const QJsonObject &node)
{
    out << YAML::BeginMap;
    if (node.contains(QStringLiteral("split"))) {
        out << YAML::Key << "split" << YAML::Value
            << node.value(QStringLiteral("split")).toString().toStdString();
        out << YAML::Key << "sizes" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const QJsonValue &s : node.value(QStringLiteral("sizes")).toArray())
            out << s.toInt();
        out << YAML::EndSeq;
        out << YAML::Key << "panes" << YAML::Value << YAML::BeginSeq;
        for (const QJsonValue &child : node.value(QStringLiteral("panes")).toArray())
            emitNested(out, base, child.toObject());
        out << YAML::EndSeq;
    } else {
        emitRoot(out, base, node);
        emitCommands(out, node);
    }
    out << YAML::EndMap;
}

} // namespace

namespace WorkspaceFile {

QStringList directories()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return {config + QStringLiteral("/superterm/workspaces"), config + QStringLiteral("/smug")};
}

QList<QPair<QString, QString>> available()
{
    QList<QPair<QString, QString>> out;
    QStringList seen;
    for (const QString &dir : directories()) {
        const QFileInfoList files = QDir(dir).entryInfoList(
            {QStringLiteral("*.yml"), QStringLiteral("*.yaml")}, QDir::Files, QDir::Name);
        for (const QFileInfo &f : files) {
            if (seen.contains(f.completeBaseName()))
                continue;
            seen << f.completeBaseName();
            out << qMakePair(f.completeBaseName(), f.absoluteFilePath());
        }
    }
    return out;
}

bool isUsersOwn(const QString &path)
{
    const QString dir = QFileInfo(path).absolutePath();
    for (const QString &d : directories()) {
        if (QDir::cleanPath(d) == dir)
            return true;
    }
    return false;
}

QString projectFileIn(const QString &dir)
{
    for (const char *name : {".superterm.yml", ".superterm.yaml", ".smug.yml", ".smug.yaml"}) {
        const QFileInfo f(QDir(dir).filePath(QLatin1String(name)));
        if (f.isFile())
            return f.absoluteFilePath();
    }
    return QString();
}

QString findProjectFile(const QString &dir)
{
    const QString home = QDir::cleanPath(QDir::homePath());
    QDir d(dir);
    if (dir.isEmpty() || !d.exists())
        return QString();
    while (true) {
        const QString here = QDir::cleanPath(d.absolutePath());
        if (here == home || d.isRoot())
            return QString();
        const QString found = projectFileIn(here);
        if (!found.isEmpty())
            return found;
        if (!d.cdUp())
            return QString();
    }
}

QStringList missingVariables(const QString &text, const QHash<QString, QString> &values)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList own = ownEnvironment(text);
    QStringList out;
    auto it = variablePattern().globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        if (!values.contains(name) && !env.contains(name) && !own.contains(name)
            && !out.contains(name))
            out << name;
    }
    return out;
}

QString expand(const QString &text, const QHash<QString, QString> &values)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList own = ownEnvironment(text);
    QString out;
    qsizetype last = 0;
    auto it = variablePattern().globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        if (own.contains(name) && !values.contains(name))
            continue;   // left in place, for the shell
        out += text.mid(last, m.capturedStart() - last);
        out += values.contains(name) ? values.value(name) : env.value(name);
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

bool parse(const QString &text, const QString &path, WorkspaceSpec &out, QString *error)
{
    YAML::Node doc;
    try {
        doc = YAML::Load(text.toStdString());
    } catch (const YAML::Exception &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
    if (!doc.IsMap()) {
        if (error)
            *error = QObject::tr("The file isn't a workspace: expected keys such as "
                                 "session, root and windows.");
        return false;
    }

    try {
        out = WorkspaceSpec();
        out.path = path;
        out.name = str(doc, "session");
        if (out.name.isEmpty())
            out.name = QFileInfo(path).completeBaseName();
        // A project's file is about its folder, as smug run there would take
        // it: no root means that folder, and a relative one is from there.
        const QString root = str(doc, "root");
        if (!isUsersOwn(path) && !path.isEmpty()) {
            const QString folder = QFileInfo(path).absolutePath();
            out.root = root.isEmpty() ? folder : resolve(folder, root);
        } else {
            out.root = root.isEmpty() ? QString() : resolve(QString(), root);
        }
        QString colour = str(doc, "colour");
        if (colour.isEmpty())
            colour = str(doc, "color");
        out.colour = QColor(colour);
        const QString profile = str(doc, "profile");
        out.beforeStart = list(doc, "before_start");
        out.stop = list(doc, "stop");
        out.sendKeysDelay = doc["sendkeys_timeout"] ? doc["sendkeys_timeout"].as<int>(0) : 0;
        const QString backend = str(doc, "backend");
        if (backend.isEmpty()) {
            const QFileInfo info(path);
            out.tmux = info.fileName() == QLatin1String(".smug.yml")
                       || info.fileName() == QLatin1String(".smug.yaml")
                       || QDir::cleanPath(info.absolutePath()) == QDir::cleanPath(directories().at(1));
        } else {
            out.tmux = backend == QLatin1String("tmux");
        }

        if (doc["env"] && doc["env"].IsMap()) {
            for (const auto &kv : doc["env"])
                out.environment << QString::fromStdString(kv.first.as<std::string>()) + QLatin1Char('=')
                                       + QString::fromStdString(kv.second.as<std::string>());
        }
        // As smug sets them, so files written for it keep working.
        out.environment << QStringLiteral("SMUG_SESSION=") + out.name
                        << QStringLiteral("SMUG_SESSION_CONFIG_PATH=") + path
                        << QStringLiteral("SUPERTERM_WORKSPACE=") + out.name;

        const YAML::Node windows = doc["windows"];
        if (windows && windows.IsSequence()) {
            for (const YAML::Node &w : windows) {
                WorkspaceSpec::Window win;
                win.name = str(w, "name");
                win.root = resolve(out.root, str(w, "root"));
                win.manual = flag(w, "manual");
                win.selected = flag(w, "selected");
                const QString winProfile = str(w, "profile").isEmpty() ? profile : str(w, "profile");

                if (!str(w, "split").isEmpty()) {
                    // superterm's nested layout.
                    win.layout = nested(w, win.root, winProfile, list(w, "commands"));
                } else {
                    // smug's: the window's pane, then one per `panes` entry,
                    // arranged by `layout`.
                    QList<QJsonObject> leaves{leaf(win.root, list(w, "commands"), winProfile,
                                                   flag(w, "exec"))};
                    if (w["panes"] && w["panes"].IsSequence()) {
                        for (const YAML::Node &p : w["panes"]) {
                            const QString paneProfile = str(p, "profile");
                            leaves << leaf(resolve(win.root, str(p, "root")), list(p, "commands"),
                                           paneProfile.isEmpty() ? winProfile : paneProfile,
                                           flag(p, "exec"));
                        }
                    }
                    win.layout = arrange(str(w, "layout"), leaves);
                }
                out.windows << win;
            }
        }
    } catch (const YAML::Exception &e) {
        if (error)
            *error = QString::fromStdString(e.what());
        return false;
    }
    return true;
}

QStringList commands(const WorkspaceSpec &spec)
{
    QStringList out = spec.beforeStart;
    for (const WorkspaceSpec::Window &w : spec.windows)
        collectCommands(w.layout, out);
    out += spec.stop;
    return out;
}

QString write(const QString &name, const QString &root, const QColor &colour,
              const QList<SavedWindow> &windows, const QString &basedOn, bool tmux)
{
    const QList<QPair<QString, QString>> blocks = topLevelBlocks(basedOn);
    const bool saysBackend = std::any_of(blocks.cbegin(), blocks.cend(), [](const auto &b) {
        return b.first == QLatin1String("backend");
    });

    YAML::Emitter head;
    head << YAML::BeginMap;
    head << YAML::Key << "session" << YAML::Value << name.toStdString();
    if (!root.isEmpty())
        head << YAML::Key << "root" << YAML::Value << relativeTo(QString(), root).toStdString();
    if (colour.isValid())
        head << YAML::Key << "colour" << YAML::Value << colour.name().toStdString();
    if (tmux && !saysBackend)
        head << YAML::Key << "backend" << YAML::Value << "tmux";
    head << YAML::EndMap;
    QString text = QString::fromStdString(head.c_str()) + QLatin1Char('\n');

    // Everything else the original said, as it said it.
    QString originalWindows;
    for (const auto &[key, block] : blocks) {
        if (key == QLatin1String("windows"))
            originalWindows = block;
        else if (key != QLatin1String("session") && key != QLatin1String("root")
                 && key != QLatin1String("colour") && key != QLatin1String("color"))
            text += block.endsWith(QLatin1Char('\n')) ? block : block + QLatin1Char('\n');
    }
    while (text.endsWith(QStringLiteral("\n\n")))
        text.chop(1);

    YAML::Emitter out;
    out << YAML::BeginSeq;
    for (const SavedWindow &w : windows) {
        // The window's root: its first pane's directory.
        QJsonObject first = w.layout;
        while (first.contains(QStringLiteral("panes")))
            first = first.value(QStringLiteral("panes")).toArray().first().toObject();
        const QString winRoot = first.value(QStringLiteral("dir")).toString();

        out << YAML::BeginMap;
        if (!w.name.isEmpty())
            out << YAML::Key << "name" << YAML::Value << w.name.toStdString();
        const QString rel = relativeTo(root, winRoot);
        if (!rel.isEmpty())
            out << YAML::Key << "root" << YAML::Value << rel.toStdString();

        const QJsonArray parts = w.layout.value(QStringLiteral("panes")).toArray();
        bool flat = !parts.isEmpty();
        for (const QJsonValue &p : parts)
            flat = flat && !p.toObject().contains(QStringLiteral("panes"));

        if (parts.isEmpty()) {
            emitCommands(out, w.layout);
        } else if (flat) {
            // smug's own form: side by side or stacked.
            out << YAML::Key << "layout" << YAML::Value
                << (w.layout.value(QStringLiteral("split")).toString() == QLatin1String("down")
                        ? "even-vertical" : "even-horizontal");
            emitCommands(out, parts.first().toObject());
            out << YAML::Key << "panes" << YAML::Value << YAML::BeginSeq;
            for (int i = 1; i < parts.size(); ++i) {
                out << YAML::BeginMap;
                emitRoot(out, winRoot, parts.at(i).toObject());
                emitCommands(out, parts.at(i).toObject());
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
        } else {
            // Nested splits: superterm's form.
            out << YAML::Key << "split" << YAML::Value
                << w.layout.value(QStringLiteral("split")).toString().toStdString();
            out << YAML::Key << "sizes" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const QJsonValue &sz : w.layout.value(QStringLiteral("sizes")).toArray())
                out << sz.toInt();
            out << YAML::EndSeq;
            out << YAML::Key << "panes" << YAML::Value << YAML::BeginSeq;
            for (const QJsonValue &p : parts)
                emitNested(out, winRoot, p.toObject());
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    text += QStringLiteral("windows:\n");
    if (!windows.isEmpty())
        text += indented(QString::fromStdString(out.c_str()), 2);

    // The original's manual windows that aren't open, kept for later.
    for (const QString &item : sequenceItems(originalWindows)) {
        const YAML::Node parsed = loadUnexpanded(item);
        if (!parsed.IsSequence() || parsed.size() == 0)
            continue;
        const QString wname = str(parsed[0], "name");
        const bool open = std::any_of(windows.cbegin(), windows.cend(),
                                      [&](const SavedWindow &sw) { return sw.name == wname; });
        if (flag(parsed[0], "manual") && !open)
            text += indented(item, 2);
    }
    if (!text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    return text;
}

} // namespace WorkspaceFile
