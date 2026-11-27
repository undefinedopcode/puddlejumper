/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "resumerules.h"

#include "procinfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QStandardPaths>

#include <optional>

namespace resumerules {

namespace {

// Claude Code's flags, from its --help (atlantis's internal/resume/builtin.go
// has the same table and the reasons). Kept: those that shape the session.
// Dropped: those that pick a session or start a task (--resume does that),
// or set up a place the resumed program is already in. Not listed, so never
// resumed: print and SDK modes, and runs that aren't a session here.
const QList<QPair<const char *, Flag>> &claudeFlags()
{
    using V = Values;
    static const QList<QPair<const char *, Flag>> flags = {
        {"--add-dir", {true, V::Many}}, {"--agent", {true, V::One}}, {"--agents", {true, V::One}},
        {"--allow-dangerously-skip-permissions", {true, V::None}}, {"--allowedTools", {true, V::Many}},
        {"--allowed-tools", {true, V::Many}}, {"--append-system-prompt", {true, V::One}},
        {"--append-system-prompt-file", {true, V::One}}, {"--autocompact", {true, V::One}},
        {"--ax-screen-reader", {true, V::None}}, {"--bare", {true, V::None}}, {"--betas", {true, V::Many}},
        {"--brief", {true, V::None}}, {"--chrome", {true, V::None}}, {"--no-chrome", {true, V::None}},
        {"--dangerously-skip-permissions", {true, V::None}}, {"-d", {true, V::Optional}},
        {"--debug", {true, V::Optional}}, {"--debug-file", {true, V::One}},
        {"--disable-slash-commands", {true, V::None}}, {"--disallowedTools", {true, V::Many}},
        {"--disallowed-tools", {true, V::Many}}, {"--effort", {true, V::One}},
        {"--exclude-dynamic-system-prompt-sections", {true, V::None}}, {"--fallback-model", {true, V::One}},
        {"--ide", {true, V::None}}, {"--mcp-config", {true, V::Many}}, {"--model", {true, V::One}},
        {"-n", {true, V::One}}, {"--name", {true, V::One}}, {"--permission-mode", {true, V::One}},
        {"--plugin-dir", {true, V::One}}, {"--plugin-url", {true, V::One}},
        {"--remote-control", {true, V::Optional}}, {"--remote-control-session-name-prefix", {true, V::One}},
        {"--restricted", {true, V::None}}, {"--safe-mode", {true, V::None}},
        {"--setting-sources", {true, V::One}}, {"--settings", {true, V::One}},
        {"--strict-mcp-config", {true, V::None}}, {"--system-prompt", {true, V::One}},
        {"--system-prompt-file", {true, V::One}}, {"--system-prompt-snapshot", {true, V::One}},
        {"--tools", {true, V::Many}}, {"--verbose", {true, V::None}},
        // Dropped.
        {"-c", {false, V::None}}, {"--continue", {false, V::None}}, {"-r", {false, V::Optional}},
        {"--resume", {false, V::Optional}}, {"--session-id", {false, V::One}},
        {"--fork-session", {false, V::None}}, {"--from-pr", {false, V::Optional}},
        {"--teleport", {false, V::Optional}}, {"--file", {false, V::Many}}, {"-w", {false, V::Optional}},
        {"--worktree", {false, V::Optional}}, {"--tmux", {false, V::Optional}},
    };
    return flags;
}

QList<Rule> builtinRules()
{
    Rule claude;
    claude.name = claude.program = QStringLiteral("claude");
    claude.source = QStringLiteral("built in");
    claude.command = {Elem{{QStringLiteral("--resume")}, {}, {}}, Elem{{}, QStringLiteral("uuid"), {}}};
    claude.hasFlags = true;
    for (const auto &f : claudeFlags())
        claude.flags.insert(QString::fromLatin1(f.first), f.second);
    Rule codex;
    codex.name = codex.program = QStringLiteral("codex");
    codex.source = claude.source;
    codex.command = {Elem{{QStringLiteral("resume")}, {}, {}}, Elem{{}, QStringLiteral("uuid"), {}}};
    Rule opencode;
    opencode.name = opencode.program = QStringLiteral("opencode");
    opencode.source = claude.source;
    opencode.command = {Elem{{QStringLiteral("--session")}, {}, {}}, Elem{{}, QStringLiteral("id"), {}}};
    return {claude, codex, opencode};
}

bool isAbs(const QString &p) { return p.startsWith(QLatin1Char('/')); }
bool homeOrAbs(const QString &p)
{
    return isAbs(p) || p == QLatin1String("~") || p.startsWith(QLatin1String("~/"));
}

// Parsing a rule; each returns an error, or "".
QString parseCommand(Rule *r, const QJsonValue &v)
{
    const QJsonArray l = v.toArray();
    if (!v.isArray() || l.isEmpty())
        return QStringLiteral("command: want a list, the program first");
    const QString prog = l.at(0).toString();
    if (prog.isEmpty() || prog.startsWith(QLatin1Char('-')) || (prog.contains(QLatin1Char('/')) && !isAbs(prog)))
        return QStringLiteral("command: the first element is the program's name or an absolute path");
    r->program = prog;
    r->command.clear();
    for (int i = 1; i < l.size(); ++i) {
        const QJsonValue x = l.at(i);
        Elem e;
        if (x.isString()) {
            e.words = {x.toString()};
        } else if (x.isArray()) {
            for (const QJsonValue &w : x.toArray()) {
                if (!w.isString())
                    return QStringLiteral("command: element %1: a list of alternatives holds strings").arg(i + 1);
                e.words << w.toString();
            }
            if (e.words.isEmpty())
                return QStringLiteral("command: element %1: an empty list of alternatives").arg(i + 1);
        } else if (x.isObject() && (x[QLatin1String("match")] == QLatin1String("uuid")
                                    || x[QLatin1String("match")] == QLatin1String("id"))) {
            e.match = x[QLatin1String("match")].toString();
        } else if (x.isObject() && x[QLatin1String("under")].isString()) {
            e.match = QStringLiteral("under");
            e.dir = x[QLatin1String("under")].toString();
            if (!homeOrAbs(e.dir))
                return QStringLiteral("command: element %1: under takes an absolute directory or one under ~").arg(i + 1);
        } else {
            return QStringLiteral("command: element %1: want a string, a list of strings, {\"match\": \"uuid\"}, "
                                  "{\"match\": \"id\"} or {\"under\": dir}").arg(i + 1);
        }
        r->command << e;
    }
    return QString();
}

QString parseFlag(const QString &name, const QJsonValue &v, Flag *out)
{
    if (!name.startsWith(QLatin1Char('-')) || name == QLatin1String("-") || name == QLatin1String("--")
        || name.contains(QLatin1Char('=')))
        return QStringLiteral("\"%1\" isn't a flag").arg(name);
    QString action;
    QJsonValue values;
    if (v.isString()) {
        action = v.toString();
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key() != QLatin1String("action") && it.key() != QLatin1String("values"))
                return QStringLiteral("\"%1\": unknown field \"%2\", want action and values").arg(name, it.key());
        action = o.value(QLatin1String("action")).toString();
        values = o.value(QLatin1String("values"));
    } else {
        return QStringLiteral("\"%1\": want \"keep\", \"drop\", false or {action, values}").arg(name);
    }
    if (action != QLatin1String("keep") && action != QLatin1String("drop"))
        return QStringLiteral("\"%1\": unknown action \"%2\", want \"keep\" or \"drop\"").arg(name, action);
    out->keep = action == QLatin1String("keep");
    out->values = Values::None;
    if (values.isUndefined() || values.isNull())
        return QString();
    if (values.isDouble() && values.toDouble() == 1)
        out->values = Values::One;
    else if (values == QLatin1String("optional"))
        out->values = Values::Optional;
    else if (values == QLatin1String("many"))
        out->values = Values::Many;
    else
        return QStringLiteral("\"%1\": values is 1, \"optional\" or \"many\"").arg(name);
    return QString();
}

QString parseDirs(const QJsonValue &v, QStringList *out)
{
    const QString err = QStringLiteral("dir: want an absolute directory or one under ~, or a list of them");
    out->clear();
    if (v.isString()) {
        if (!homeOrAbs(v.toString()))
            return err;
        *out = {v.toString()};
        return QString();
    }
    if (!v.isArray() || v.toArray().isEmpty())
        return err;
    for (const QJsonValue &d : v.toArray()) {
        if (!d.isString() || !homeOrAbs(d.toString()))
            return err;
        *out << d.toString();
    }
    return QString();
}

QString parseRule(const QString &name, const QJsonValue &v, Rule *r)
{
    if (!v.isObject())
        return QStringLiteral("want false or an object with command");
    r->name = name;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QString err;
        if (it.key() == QLatin1String("command")) {
            err = parseCommand(r, it.value());
        } else if (it.key() == QLatin1String("flags")) {
            r->hasFlags = true;
            if (!it.value().isObject())
                return QStringLiteral("flags: want an object keyed by flag");
            const QJsonObject f = it.value().toObject();
            for (auto fit = f.begin(); fit != f.end(); ++fit) {
                Flag flag;
                if (const QString e = parseFlag(fit.key(), fit.value(), &flag); !e.isEmpty())
                    return QStringLiteral("flags: ") + e;
                r->flags.insert(fit.key(), flag);
            }
        } else if (it.key() == QLatin1String("dir")) {
            err = parseDirs(it.value(), &r->dirs);
        } else {
            err = QStringLiteral("unknown field \"%1\", want command, flags or dir").arg(it.key());
        }
        if (!err.isEmpty())
            return err;
    }
    if (r->program.isEmpty())
        return QStringLiteral("no command");
    return QString();
}

QString applyPart(Rule *r, const QString &part, const QJsonValue &v, QString *change)
{
    if (part == QLatin1String("dir")) {
        QStringList dirs;
        if (const QString e = parseDirs(v, &dirs); !e.isEmpty())
            return e;
        r->dirs = dirs;
        *change = QStringLiteral("dir: ") + dirs.join(QStringLiteral(", "));
        return QString();
    }
    if (part == QLatin1String("command")) {
        *change = QStringLiteral("command changed");
        return parseCommand(r, v);
    }
    if (part.startsWith(QLatin1String("flags."))) {
        const QString flag = part.mid(6);
        r->hasFlags = true;
        if (v.isBool() && !v.toBool()) {
            if (!flag.startsWith(QLatin1Char('-')))
                return QStringLiteral("\"%1\" isn't a flag").arg(flag);
            r->flags.remove(flag);
            *change = flag + QStringLiteral(" removed");
            return QString();
        }
        Flag f;
        if (const QString e = parseFlag(flag, v, &f); !e.isEmpty())
            return QStringLiteral("flags: ") + e;
        r->flags.insert(flag, f);
        *change = flag + (f.keep ? QStringLiteral(" kept") : QStringLiteral(" dropped"));
        return QString();
    }
    return QStringLiteral("unknown part \"%1\", want dir, command or flags.<flag>").arg(part);
}

bool strictlyUnder(const QString &p, QString dir)
{
    while (dir.size() > 1 && dir.endsWith(QLatin1Char('/')))
        dir.chop(1);
    return p.startsWith(dir + QLatin1Char('/')) && p.size() > dir.size() + 1;
}

QString realPath(const QString &p)
{
    const QString c = QFileInfo(p).canonicalFilePath();
    return c;
}

QString baseName(QString arg)
{
    if (arg.startsWith(QLatin1Char('-')))
        arg = arg.mid(1);
    return arg.mid(arg.lastIndexOf(QLatin1Char('/')) + 1);
}

// The process is the rule's program: its arguments after the program.
std::optional<QStringList> programArgs(const Rule &r, const QStringList &a, const Process &p)
{
    const auto is = [&](const QString &arg) {
        if (isAbs(r.program)) {
            if (arg == r.program)
                return true;
            const QString x = realPath(arg), y = realPath(r.program);
            return !x.isEmpty() && x == y;
        }
        return baseName(arg) == r.program;
    };
    if (is(a.at(0)) || (!isAbs(r.program) && p.name && p.name(p.pid) == r.program)
        || (isAbs(r.program) && p.executable && p.executable(p.pid) == r.program))
        return a.mid(1);
    static const QStringList interpreters = {QStringLiteral("node"), QStringLiteral("bun"), QStringLiteral("deno"),
                                             QStringLiteral("python"), QStringLiteral("python3")};
    // A script: its interpreter, then it ("/bin/sh /path/script ...").
    if (a.size() >= 2 && (interpreters.contains(baseName(a.at(0))) || isShell(a.at(0))) && is(a.at(1)))
        return a.mid(2);
    return std::nullopt;
}

bool wrapped(const Process &p)
{
    if (p.pid == p.leader)
        return false;
    int at = p.pid;
    for (int i = 0; i < 32; ++i) {
        const int up = p.parent(at);
        if (up == p.leader)
            return false;
        if (up <= 1 || !isShell(p.name(up)))
            return true;
        at = up;
    }
    return true;
}

// The arguments of the flags the rule keeps, as they were; `other` says
// whether anything was left out (not a flag or a flag's value, or "--").
QStringList readFlags(const Rule &r, const QStringList &a, bool *other, QString *reason)
{
    QStringList kept;
    *other = false;
    reason->clear();
    for (int i = 0; i < a.size(); ++i) {
        const QString &x = a.at(i);
        if (x == QLatin1String("--")) {
            *other = true;
            return kept;
        }
        if (!x.startsWith(QLatin1Char('-')) || x == QLatin1String("-")) {
            *other = true;
            continue;
        }
        QString name = x;
        bool hasEq = false;
        if (x.startsWith(QLatin1String("--")) && x.contains(QLatin1Char('='))) {
            name = x.left(x.indexOf(QLatin1Char('=')));
            hasEq = true;
        }
        const auto it = r.flags.constFind(name);
        if (it == r.flags.constEnd()) {
            *reason = FlagNotListed;
            return {};
        }
        int j = i + 1;
        const auto dash = [&](int k) { return a.at(k).startsWith(QLatin1Char('-')); };
        switch (it->values) {
        case Values::None:
            if (hasEq) {
                *reason = FlagValue;
                return {};
            }
            break;
        case Values::One:
            if (!hasEq) {
                if (j >= a.size()) {
                    *reason = FlagValue;
                    return {};
                }
                ++j;
            }
            break;
        case Values::Optional:
            if (!hasEq && j < a.size() && !dash(j))
                ++j;
            break;
        case Values::Many:
            if (!hasEq && (j >= a.size() || dash(j))) {
                *reason = FlagValue;
                return {};
            }
            while (j < a.size() && !dash(j))
                ++j;
            break;
        }
        if (it->keep)
            kept << a.mid(i, j - i);
        i = j - 1;
    }
    return kept;
}

const QRegularExpression &uuidRe()
{
    static const QRegularExpression re(QStringLiteral(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return re;
}

} // namespace

QString Rule::commandText() const
{
    QStringList parts{program};
    for (const Elem &e : command) {
        if (e.match == QLatin1String("uuid"))
            parts << QStringLiteral("<uuid>");
        else if (e.match == QLatin1String("id"))
            parts << QStringLiteral("<id>");
        else if (e.match == QLatin1String("under"))
            parts << QStringLiteral("<path under %1>").arg(e.dir);
        else
            parts << e.words.join(QLatin1Char('|'));
    }
    return parts.join(QLatin1Char(' '));
}

bool isShell(const QString &name)
{
    static const QStringList shells = {
        QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish"), QStringLiteral("nu"),
        QStringLiteral("sh"), QStringLiteral("ash"), QStringLiteral("dash"), QStringLiteral("ksh"),
        QStringLiteral("mksh"), QStringLiteral("oksh"), QStringLiteral("pdksh"), QStringLiteral("yash"),
        QStringLiteral("tcsh"), QStringLiteral("csh")};
    return shells.contains(baseName(name));
}

Process Process::live(int pid, int leader)
{
    Process p;
    p.pid = pid;
    p.leader = leader;
    p.arguments = procinfo::arguments;
    p.name = procinfo::name;
    p.executable = procinfo::executable;
    p.parent = procinfo::parent;
    return p;
}

Rules Rules::incomplete()
{
    Rules rs;
    rs.isIncomplete = true;
    rs.m_home = QDir::homePath();
    return rs;
}

Rules Rules::build(const QList<Op> &ops, const QString &home)
{
    Rules rs;
    rs.m_home = home;
    struct Entry
    {
        std::optional<Rule> rule;
        bool builtin = false;
        QString removed, broken;
    };
    QMap<QString, Entry> entries;   // by name
    for (const Rule &r : builtinRules())
        entries.insert(r.name, Entry{r, true, {}, {}});
    QString defaultsOff;
    QList<Op> partial;
    const auto warn = [&rs](const QString &src, const QString &msg) { rs.diagnostics << src + QStringLiteral(": ") + msg; };
    static const QRegularExpression ruleName(QStringLiteral("^[A-Za-z0-9_-]+$"));

    for (const Op &op : ops) {
        const QString key = op.key.startsWith(QLatin1String("resume.")) ? op.key.mid(7) : op.key;
        if (key == QLatin1String("defaults")) {
            if (!op.value.isBool()) {
                warn(op.source, QStringLiteral("setting: resume.defaults: want true or false"));
                continue;
            }
            defaultsOff = op.value.toBool() ? QString() : op.source;
            continue;
        }
        if (!key.startsWith(QLatin1String("trust.")) || key.size() == 6) {
            warn(op.source, QStringLiteral("setting: %1: unknown setting").arg(op.key));
            continue;
        }
        const QString rest = key.mid(6);
        const int dot = rest.indexOf(QLatin1Char('.'));
        const QString name = dot < 0 ? rest : rest.left(dot);
        const QString part = dot < 0 ? QString() : rest.mid(dot + 1);
        if (!ruleName.match(name).hasMatch()) {
            warn(op.source, QStringLiteral("setting: resume.trust.%1: a rule's name is letters, digits, _ and -").arg(name));
            continue;
        }
        if (part.isEmpty()) {
            if (op.value.isBool() && !op.value.toBool()) {
                entries[name] = Entry{std::nullopt, false, op.source, {}};
                continue;
            }
            Rule r;
            if (const QString e = parseRule(name, op.value, &r); !e.isEmpty()) {
                warn(op.source, QStringLiteral("setting: resume.trust.%1: %2, so this rule is ignored").arg(name, e));
                entries[name] = Entry{std::nullopt, false, {}, op.source};
                continue;
            }
            r.source = op.source;
            entries[name] = Entry{r, false, {}, {}};
            continue;
        }
        Entry *e = entries.contains(name) ? &entries[name] : nullptr;
        if (!e || !e->rule) {
            if (e && !e->broken.isEmpty())
                continue;
            warn(op.source, QStringLiteral("setting: resume.trust.%1: there is no rule \"%2\" to change").arg(rest, name));
            continue;
        }
        if (e->builtin)
            partial << op;
        QString change;
        if (const QString err = applyPart(&*e->rule, part, op.value, &change); !err.isEmpty()) {
            warn(op.source, QStringLiteral("setting: resume.trust.%1: %2, so this rule is ignored").arg(name, err));
            e->broken = op.source;
            e->rule.reset();
            continue;
        }
        if (e->builtin)
            e->rule->changes << change + QStringLiteral(" (") + op.source + QLatin1Char(')');
    }
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const Entry &e = it.value();
        if (!e.removed.isEmpty())
            rs.off << QStringLiteral("%1 removed at %2").arg(it.key(), e.removed);
        else if (!e.broken.isEmpty())
            rs.off << QStringLiteral("%1 left out for a mistake, see the warning for %2").arg(it.key(), e.broken);
        else if (e.builtin && !defaultsOff.isEmpty())
            rs.off << QStringLiteral("%1 off because resume.defaults is false at %2").arg(it.key(), defaultsOff);
        else if (e.rule)
            rs.list << *e.rule;
    }
    if (!defaultsOff.isEmpty())
        for (const Op &op : std::as_const(partial))
            warn(op.source, QStringLiteral("setting: %1: resume.defaults is false, so the built-in rule this changes "
                                           "is not in force").arg(op.key));
    return rs;
}

QString Rules::expand(const QString &p) const
{
    if (p == QLatin1String("~"))
        return m_home;
    if (p.startsWith(QLatin1String("~/")))
        return QDir::cleanPath(m_home + p.mid(1));
    return QDir::cleanPath(p);
}

bool Rules::matches(const Rule &r, const QStringList &argv) const
{
    if (argv.at(0) != r.program)
        return false;   // a name matches only that name, never a path
    for (int i = 0; i < r.command.size(); ++i) {
        const Elem &e = r.command.at(i);
        const QString &s = argv.at(1 + i);
        if (e.match == QLatin1String("uuid")) {
            if (!uuidRe().match(s).hasMatch())
                return false;
        } else if (e.match == QLatin1String("id")) {
            static const QRegularExpression idRe(QStringLiteral("^[A-Za-z0-9_]{1,128}$"));
            if (!idRe.match(s).hasMatch())
                return false;
        } else if (e.match == QLatin1String("under")) {
            if (!isAbs(s) || QDir::cleanPath(s) != s || !strictlyUnder(s, expand(e.dir)))
                return false;
        } else if (!e.words.contains(s)) {
            return false;
        }
    }
    return true;
}

const Rule *Rules::find(const QStringList &argv, int *n) const
{
    for (const Rule &r : list) {
        const int need = int(r.command.size()) + 1;
        if (argv.size() >= need && matches(r, argv)) {
            *n = need;
            return &r;
        }
    }
    return nullptr;
}

bool Rules::dirAllowed(const Rule &r, const QString &cwdIn, bool followLinks) const
{
    if (r.dirs.isEmpty())
        return true;
    if (cwdIn.isEmpty())
        return false;
    QString cwd = QDir::cleanPath(cwdIn);
    if (followLinks)
        if (const QString real = realPath(cwd); !real.isEmpty())
            cwd = real;
    for (const QString &d : r.dirs) {
        QString dir = expand(d);
        if (followLinks)
            if (const QString real = realPath(dir); !real.isEmpty())
                dir = real;
        if (cwd == dir || strictlyUnder(cwd, dir))
            return true;
    }
    return false;
}

Verdict Rules::check(const QStringList &argv, const QString &cwd, const Process &p) const
{
    Verdict v;
    if (isIncomplete) {
        v.reason = ConfigIncomplete;
        return v;
    }
    int n = 0;
    const Rule *r = argv.isEmpty() ? nullptr : find(argv, &n);
    if (!r) {
        v.reason = NoRule;
        return v;
    }
    v.rule = r->name;
    const QStringList a = p.arguments(p.pid);
    if (a.isEmpty()) {
        v.reason = LaunchUnreadable;
        return v;
    }
    const auto args = programArgs(*r, a, p);
    if (!args) {
        v.reason = ProgramDiffers;
        return v;
    }
    if (wrapped(p)) {
        v.reason = Wrapped;
        return v;
    }
    QStringList command = argv.mid(0, n);
    if (r->hasFlags) {
        bool other = false;
        QString reason;
        const QStringList kept = readFlags(*r, *args, &other, &reason);
        if (!reason.isEmpty()) {
            v.reason = reason;
            return v;
        }
        readFlags(*r, argv.mid(n), &other, &reason);
        if (!reason.isEmpty() || other) {
            v.reason = FlagsDiffer;
            return v;
        }
        command << kept;
    } else if (argv.size() != n) {
        v.reason = FlagsDiffer;
        return v;
    }
    if (!dirAllowed(*r, cwd, false)) {
        v.reason = DirNotAllowed;
        return v;
    }
    v.trusted = true;
    v.command = command;
    return v;
}

Verdict Rules::checkSaved(const QStringList &command, const QString &cwd) const
{
    Verdict v;
    if (isIncomplete) {
        v.reason = ConfigIncomplete;
        return v;
    }
    int n = 0;
    const Rule *r = command.isEmpty() ? nullptr : find(command, &n);
    if (!r) {
        v.reason = NoRule;
        return v;
    }
    v.rule = r->name;
    for (int i = 0; i < r->command.size(); ++i) {
        const Elem &e = r->command.at(i);
        if (e.match != QLatin1String("under"))
            continue;
        const QString real = realPath(command.at(1 + i)), dir = realPath(expand(e.dir));
        if (real.isEmpty() || dir.isEmpty() || !strictlyUnder(real, dir)) {
            v.reason = NoRule;
            return v;
        }
    }
    if (r->hasFlags) {
        bool other = false;
        QString reason;
        const QStringList kept = readFlags(*r, command.mid(n), &other, &reason);
        if (!reason.isEmpty() || other || kept.size() != command.size() - n) {
            v.reason = FlagsDiffer;
            return v;
        }
    } else if (command.size() != n) {
        v.reason = FlagsDiffer;
        return v;
    }
    if (!dirAllowed(*r, cwd, true)) {
        v.reason = DirNotAllowed;
        return v;
    }
    v.trusted = true;
    v.command = command;
    return v;
}

QString path()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/puddlejumper/resume-rules.json");
}

Rules load(const QString &file)
{
    QFile f(file);
    if (!f.exists())
        return Rules::build({}, QDir::homePath());
    QJsonParseError err;
    const QJsonDocument doc = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll(), &err) : QJsonDocument();
    const QString base = QFileInfo(file).fileName();
    if (!doc.isArray()) {
        Rules rs = Rules::incomplete();
        rs.diagnostics << base + QStringLiteral(": not a JSON list of [key, value] pairs (")
                              + (f.isOpen() ? err.errorString() : f.errorString())
                              + QStringLiteral("), so nothing is trusted to resume");
        return rs;
    }
    QList<Op> ops;
    QStringList bad;
    const QJsonArray list = doc.array();
    for (int i = 0; i < list.size(); ++i) {
        const QJsonArray pair = list.at(i).toArray();
        const QString source = QStringLiteral("%1:#%2").arg(base).arg(i + 1);
        if (pair.size() != 2 || !pair.at(0).isString()) {
            bad << source + QStringLiteral(": want [key, value]");
            continue;
        }
        ops << Op{pair.at(0).toString(), pair.at(1), source};
    }
    Rules rs = Rules::build(ops, QDir::homePath());
    rs.diagnostics = bad + rs.diagnostics;
    return rs;
}

namespace {
std::optional<Rules> &currentRules()
{
    static std::optional<Rules> rules;
    return rules;
}
} // namespace

const Rules &current()
{
    if (!currentRules())
        reload();
    return *currentRules();
}

void reload()
{
    currentRules() = load();
    for (const QString &d : std::as_const(currentRules()->diagnostics))
        qWarning("resume rules: %s", qPrintable(d));
}

const Rule &claudeRule()
{
    static const Rule rule = builtinRules().first();
    return rule;
}

QStringList report(const Rule &r, const QStringList &commandPart, const QStringList &args, QString *reason)
{
    bool other = false;
    QString why;
    const QStringList kept = readFlags(r, args, &other, &why);
    if (reason)
        *reason = why;
    if (!why.isEmpty())
        return {};
    return commandPart + kept;
}

} // namespace resumerules
