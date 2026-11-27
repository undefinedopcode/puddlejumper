/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

/*
 * Which programs a restored local tab may start again (program resume,
 * OSC 7577): trust rules, as Rex's resume.trust settings and atlantis have
 * them (atlantis's internal/resume is the same design, in Go).
 *
 * A rule names a command (claude --resume <uuid>), the flags the program may
 * have been started with and whether each comes back, and the directories
 * it may be resumed in. A program's report is never taken as it is: its
 * command must match a rule's, the process that reported must be the program
 * the command starts and run straight under the tab's shell, and the command
 * a restore runs is the reported command part followed by the flags the rule
 * keeps from the process's own command line.
 *
 * Built in: claude, codex and opencode. Changes come from
 * ~/.config/puddlejumper/resume-rules.json, a list of Rex's rex.set calls
 * as [key, value] pairs, applied in order:
 *
 *   [
 *     ["resume.trust.claude.dir", "~/code"],
 *     ["resume.trust.claude.flags.--dangerously-skip-permissions", false],
 *     ["resume.trust.codex", false],
 *     ["resume.trust.nvim", {"command": ["nvim", "-S",
 *                            {"under": "~/.local/state/nvim/sessions"}]}],
 *     ["resume.defaults", false]
 *   ]
 *
 * In a command, {"match": "uuid"}, {"match": "id"} and {"under": dir} stand
 * for rex.match.uuid, rex.match.id and rex.match.under(dir); a flag is
 * "keep", "drop", or {"action": "keep", "values": 1 | "optional" | "many"}.
 */
namespace resumerules {

enum class Values { None, One, Optional, Many };

struct Flag
{
    bool keep = true;
    Values values = Values::None;
};

struct Elem
{
    QStringList words;   // any one of these exactly
    QString match;       // or "uuid", "id", "under"
    QString dir;         // for "under", as written
};

struct Rule
{
    QString name;
    QString program;     // a name, or an absolute path
    QList<Elem> command; // after the program
    QHash<QString, Flag> flags;
    bool hasFlags = false;
    QStringList dirs;    // as written; none: anywhere
    QString source;      // "built in", or where it was set
    QStringList changes; // what the file changed in a built-in rule

    QString commandText() const;
};

// Why a record isn't trusted (Rex's untrusted_reason values).
inline const QString NoRule = QStringLiteral("no_rule");
inline const QString ProgramDiffers = QStringLiteral("program_differs");
inline const QString Wrapped = QStringLiteral("wrapped");
inline const QString FlagNotListed = QStringLiteral("flag_not_listed");
inline const QString FlagValue = QStringLiteral("flag_value");
inline const QString FlagsDiffer = QStringLiteral("flags_differ");
inline const QString DirNotAllowed = QStringLiteral("dir_not_allowed");
inline const QString LaunchUnreadable = QStringLiteral("launch_unreadable");
inline const QString ConfigIncomplete = QStringLiteral("config_incomplete");

struct Verdict
{
    bool trusted = false;
    QString rule;
    QString reason;      // when not trusted
    QStringList command; // what a restore runs
};

// How the reporting process is looked at (procinfo's by default).
struct Process
{
    int pid = 0;
    int leader = 0;      // the tab's shell
    std::function<QStringList(int)> arguments;
    std::function<QString(int)> name;
    std::function<QString(int)> executable;
    std::function<int(int)> parent;

    static Process live(int pid, int leader);
};

struct Op
{
    QString key;
    QJsonValue value;
    QString source;
};

class Rules
{
public:
    // The built-in rules with the file's changes. `home` stands for "~".
    static Rules build(const QList<Op> &ops, const QString &home);
    // Trusts nothing: the file didn't load.
    static Rules incomplete();

    // A live record: argv as reported, cwd its directory.
    Verdict check(const QStringList &argv, const QString &cwd, const Process &p) const;
    // A saved command, at a restore (links followed).
    Verdict checkSaved(const QStringList &command, const QString &cwd) const;

    QList<Rule> list;          // by name
    QStringList off;           // rules not in force, and why
    QStringList diagnostics;   // warnings, "source: message"
    bool isIncomplete = false;

private:
    const Rule *find(const QStringList &argv, int *n) const;
    bool matches(const Rule &r, const QStringList &argv) const;
    bool dirAllowed(const Rule &r, const QString &cwd, bool followLinks) const;
    QString expand(const QString &p) const;
    QString m_home;
};

// The file's path, and the rules from it (built-in ones when it's missing;
// incomplete() when it can't be read as rules).
QString path();
Rules load(const QString &file = path());

// The rules in force (loaded once; reload() reads the file again).
const Rules &current();
void reload();

// Whether a process name is a shell's ("-zsh" for a login one).
bool isShell(const QString &name);

// The built-in Claude Code rule, and what a reporter for a process would
// report: the command part, then the flags the rule keeps from its
// arguments (after the program). Empty with the reason if it can't.
const Rule &claudeRule();
QStringList report(const Rule &r, const QStringList &commandPart, const QStringList &args,
                   QString *reason = nullptr);

} // namespace resumerules
