/*
 * test-programresume — the OSC 7577 parser and records (Program Resume
 * Protocol 0.5), and the trust rules, with the cases atlantis's
 * internal/resume tests. Prints each failure; exits 1 if there was any.
 */

#include "programresume.h"
#include "resumerules.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <unistd.h>

using namespace resumerules;

namespace {

int failures = 0;

void check(bool ok, const char *what, int line)
{
    if (!ok) {
        QTextStream(stderr) << "FAIL line " << line << ": " << what << "\n";
        ++failures;
    }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

const QString kToken = QStringLiteral("abcdefghijklmnopqrstuvwxyz012345");
const QString kSid = QStringLiteral("4394937c-b07a-4bd5-b6d0-034e7091ff1f");

QString b64(const QString &s, bool pad = true)
{
    QByteArray b = s.toUtf8().toBase64();
    if (!pad)
        while (b.endsWith('='))
            b.chop(1);
    return QString::fromLatin1(b);
}

QString sign(const QString &body, const QString &token = kToken)
{
    return body + QStringLiteral(":mac=")
           + QString::fromLatin1(QCryptographicHash::hash((body + QLatin1Char(':') + token).toUtf8(),
                                                          QCryptographicHash::Sha256).toHex());
}

QString setBody(int pid, qint64 time, const QStringList &argv, const QString &extra = QString())
{
    QStringList parts;
    for (const QString &a : argv)
        parts << b64(a);
    return QStringLiteral("pid=%1:time=%2:argv=%3").arg(pid).arg(time).arg(parts.join(QLatin1Char(','))) + extra;
}

void testParse()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    ProgramResume::Report r;
    using V = ProgramResume::Verdict;
    const auto parse = [&](const QString &body) { return ProgramResume::parse(body, kToken, now, &r); };

    CHECK(parse(sign(setBody(42, now, {"claude", "--resume", kSid}, ":cwd=" + b64("/tmp")))) == V::Set);
    CHECK(r.pid == 42 && r.argv == QStringList({"claude", "--resume", kSid}) && r.cwd == "/tmp");
    // Unpadded base64 is fine.
    CHECK(parse(sign(QStringLiteral("pid=42:time=%1:argv=%2").arg(now).arg(b64("ab", false)))) == V::Set
          && r.argv == QStringList{"ab"});
    CHECK(parse(sign(QStringLiteral("pid=42:time=%1:clear=1").arg(now))) == V::Clear);

    // Ignored: unsigned, signed with another token, uppercase mac, stale, '?'.
    CHECK(parse(setBody(42, now, {"x"})) == V::Ignore);
    CHECK(parse(sign(setBody(42, now, {"x"}), QStringLiteral("other"))) == V::Ignore);
    CHECK(parse(sign(setBody(42, now, {"x"})).toUpper()) == V::Ignore);
    CHECK(parse(sign(setBody(42, now - 120, {"x"}))) == V::Ignore);
    CHECK(parse(QStringLiteral("?")) == V::Ignore);
    CHECK(ProgramResume::parse(sign(setBody(42, now, {"x"})), QString(), now, &r) == V::Ignore);
    CHECK(parse(sign(QStringLiteral("pid=042:time=%1:argv=%2").arg(now).arg(b64("x")))) == V::Ignore);

    // Rejected: malformed once authenticated.
    CHECK(parse(sign(setBody(42, now, {"x"}, ":cwd=" + b64("relative")))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"bin/x"}))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"-x"}))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"x"}, ":need=argv,frob"))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"x"}, ":clear=1"))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"a\tb"}))) == V::Reject);
    CHECK(parse(sign(setBody(42, now, {"x"}, ":pid=43"))) == V::Reject);
    CHECK(parse(sign(QStringLiteral("pid=42:time=%1:argv=%%%%").arg(now))) == V::Reject);
    // Unknown keys are let be; a known need is fine.
    CHECK(parse(sign(setBody(42, now, {"x"}, ":frob=1:need=argv"))) == V::Set);
}

void testRecords()
{
    // This process, in its own session: getsid(0) leads it.
    const int me = int(::getpid()), leader = int(::getsid(0));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    ProgramResume pr(kToken);
    CHECK(ProgramResume::newToken().size() == 32 && ProgramResume::newToken() != ProgramResume::newToken());
    CHECK(pr.apply(sign(setBody(me, now, {"claude", "--resume", kSid})), leader));
    CHECK(pr.records(leader).size() == 1);
    // An older report than one taken is ignored; a newer one replaces it.
    CHECK(!pr.apply(sign(setBody(me, now - 5, {"older"})), leader));
    CHECK(pr.apply(sign(setBody(me, now + 1, {"newer"})), leader));
    CHECK(pr.records(leader).first().argv == QStringList{"newer"});
    // Not this tab's: another session.
    CHECK(!pr.apply(sign(setBody(me, now + 2, {"x"})), leader + 100000));
    // A process that doesn't exist.
    CHECK(!pr.apply(sign(setBody(999999, now, {"x"})), leader));
    // A rejected report removes its owner's record; clear does too.
    CHECK(pr.apply(sign(setBody(me, now + 3, {"bin/x"})), leader) && pr.records(leader).isEmpty());
    CHECK(pr.apply(sign(setBody(me, now + 4, {"y"})), leader));
    CHECK(pr.apply(sign(QStringLiteral("pid=%1:time=%2:clear=1").arg(me).arg(now + 5)), leader));
    CHECK(pr.records(leader).isEmpty());
}

// A process tree: pid → argv, parent.
struct Fake
{
    QHash<int, QPair<QStringList, int>> procs;
    Process process(int pid, int leader) const
    {
        Process p;
        p.pid = pid;
        p.leader = leader;
        const auto procsCopy = procs;
        p.arguments = [procsCopy](int q) { return procsCopy.value(q).first; };
        p.name = [procsCopy](int q) {
            const QStringList a = procsCopy.value(q).first;
            return a.isEmpty() ? QString() : a.first().mid(a.first().lastIndexOf('/') + 1);
        };
        p.executable = [](int) { return QString(); };
        p.parent = [procsCopy](int q) { return procsCopy.value(q).second; };
        return p;
    }
};

Fake tree(const QStringList &claude)
{
    Fake f;
    f.procs.insert(10, {{"-zsh"}, 1});
    f.procs.insert(20, {claude, 10});
    return f;
}

void testRules()
{
    const Rules rs = Rules::build({}, QStringLiteral("/home/u"));
    const QStringList report{"claude", "--resume", kSid, "--model", "opus"};
    Verdict v = rs.check(report, "/home/u/code", tree({"claude", "--model", "opus", "fix it"}).process(20, 10));
    CHECK(v.trusted && v.rule == "claude" && v.command == report);
    // The command is the rule's: the process's kept flags.
    v = rs.check({"claude", "--resume", kSid}, "/x",
                 tree({"node", "/opt/homebrew/bin/claude", "-c", "--add-dir", "/a", "/b", "--permission-mode=plan"})
                     .process(20, 10));
    CHECK(v.trusted && v.command == QStringList({"claude", "--resume", kSid, "--add-dir", "/a", "/b", "--permission-mode=plan"}));

    const auto reason = [&](const QStringList &argv, const Process &p) { return rs.check(argv, "/x", p).reason; };
    CHECK(reason({"claude", "--resume", "nope"}, tree({"claude"}).process(20, 10)) == NoRule);
    CHECK(reason({"/usr/bin/claude", "--resume", kSid}, tree({"claude"}).process(20, 10)) == NoRule);
    CHECK(reason(report, tree({"claude", "-p", "hi"}).process(20, 10)) == FlagNotListed);
    CHECK(reason(report, tree({"claude", "--verbose=1"}).process(20, 10)) == FlagValue);
    CHECK(reason(report, tree({"claude", "--model"}).process(20, 10)) == FlagValue);
    CHECK(reason(report, tree({"vim"}).process(20, 10)) == ProgramDiffers);
    Fake sandbox;
    sandbox.procs.insert(10, {{"zsh"}, 1});
    sandbox.procs.insert(15, {{"sandbox-exec"}, 10});
    sandbox.procs.insert(20, {{"claude"}, 15});
    CHECK(reason(report, sandbox.process(20, 10)) == Wrapped);
    CHECK(reason({"claude", "--resume", kSid, "do this"}, tree({"claude"}).process(20, 10)) == FlagsDiffer);
    CHECK(reason({"claude", "--resume", kSid, "--evil"}, tree({"claude"}).process(20, 10)) == FlagsDiffer);
    CHECK(reason(report, Fake().process(20, 10)) == LaunchUnreadable);
    Fake nested;
    nested.procs.insert(10, {{"zsh"}, 1});
    nested.procs.insert(15, {{"/bin/bash"}, 10});
    nested.procs.insert(20, {{"claude"}, 15});
    CHECK(rs.check(report.mid(0, 3), "/x", nested.process(20, 10)).trusted);
    CHECK(rs.check({"codex", "resume", kSid}, "/x", tree({"codex", "--yolo"}).process(20, 10)).trusted);
    CHECK(reason({"codex", "resume", kSid, "-x"}, tree({"codex"}).process(20, 10)) == FlagsDiffer);
    // A script under its interpreter.
    CHECK(Rules::build({{"resume.trust.s", QJsonObject{{"command", QJsonArray{"/opt/s", "go"}}}, "f:#1"}}, "/h")
              .check({"/opt/s", "go"}, "/", tree({"/bin/sh", "/opt/s", "go"}).process(20, 10)).trusted);

    // Changes, as Rex's examples.
    const QList<Op> ops = {
        {"resume.trust.codex", false, "f:#1"},
        {"resume.trust.claude.dir", "~/code", "f:#2"},
        {"resume.trust.claude.flags.--dangerously-skip-permissions", false, "f:#3"},
    };
    const Rules changed = Rules::build(ops, "/home/u");
    CHECK(changed.list.size() == 2 && changed.list.at(0).name == "claude" && changed.list.at(1).name == "opencode");
    CHECK(changed.off == QStringList{"codex removed at f:#1"});
    CHECK(changed.list.at(0).changes.size() == 2);
    const Process plain = tree({"claude"}).process(20, 10);
    CHECK(changed.check({"claude", "--resume", kSid}, "/home/u/code/x",
                        tree({"claude", "--dangerously-skip-permissions"}).process(20, 10)).reason == FlagNotListed);
    CHECK(changed.check({"claude", "--resume", kSid}, "/home/u/code/x", plain).trusted);
    CHECK(changed.check({"claude", "--resume", kSid}, "/home/u/codex", plain).reason == DirNotAllowed);

    // A mistake leaves the built-in rule out, with a warning.
    const Rules mistake = Rules::build({{"resume.trust.claude.flags.--model", "keeep", "f:#1"}}, "/h");
    CHECK(mistake.list.size() == 2 && mistake.diagnostics.size() == 1
          && mistake.diagnostics.first().contains("unknown action \"keeep\", want \"keep\" or \"drop\", so this rule is ignored"));

    // defaults off: built-ins go (a changed one with a warning); one set in full stays.
    const Rules own = Rules::build({
        {"resume.trust.codex.dir", "/w", "f:#1"},
        {"resume.defaults", false, "f:#2"},
        {"resume.trust.claude", QJsonObject{
            {"command", QJsonArray{"claude", "--resume", QJsonObject{{"match", "uuid"}}}},
            {"flags", QJsonObject{{"--model", QJsonObject{{"action", "keep"}, {"values", 1}}}, {"--continue", "drop"}}},
        }, "f:#3"},
    }, "/h");
    CHECK(own.list.size() == 1 && own.list.first().name == "claude" && own.off.size() == 2 && own.diagnostics.size() == 1);
    v = own.check({"claude", "--resume", kSid}, "/", tree({"claude", "--model", "x", "--continue"}).process(20, 10));
    CHECK(v.trusted && v.command == QStringList({"claude", "--resume", kSid, "--model", "x"}));
    CHECK(own.check({"claude", "--resume", kSid}, "/", tree({"claude", "--verbose"}).process(20, 10)).reason == FlagNotListed);

    // Saved commands, at a restore.
    CHECK(rs.checkSaved(report, "/").trusted);
    CHECK(Rules::build({{"resume.trust.claude.flags.--model", "drop", "f:#1"}}, "/h").checkSaved(report, "/").reason
          == FlagsDiffer);
    CHECK(Rules::incomplete().checkSaved(report, "/").reason == ConfigIncomplete);

    // report(): what a reporter gives for claude.
    QString why;
    CHECK(resumerules::report(claudeRule(), {"claude", "--resume", kSid},
                              {"--model", "opus", "-c", "--debug", "api", "do it", "--", "--model", "x"}, &why)
          == QStringList({"claude", "--resume", kSid, "--model", "opus", "--debug", "api"}));
    resumerules::report(claudeRule(), {}, {"-p"}, &why);
    CHECK(why == FlagNotListed);
}

void testUnderAndFile()
{
    QTemporaryDir home;
    const QString sessions = home.path() + "/.local/state/nvim/sessions";
    QDir().mkpath(sessions);
    QFile(sessions + "/work.vim").open(QIODevice::WriteOnly);
    QFile::link("/etc/passwd", sessions + "/sneaky.vim");
    const Rules rs = Rules::build({{"resume.trust.nvim", QJsonObject{
        {"command", QJsonArray{"nvim", "-S", QJsonObject{{"under", "~/.local/state/nvim/sessions"}}}}}, "f:#1"}},
        home.path());
    const Process p = tree({"nvim", "-S", "x"}).process(20, 10);
    const QStringList good{"nvim", "-S", sessions + "/work.vim"};
    CHECK(rs.check(good, "/", p).trusted);
    for (const QString &bad : {sessions, sessions + "/../x.vim", QStringLiteral("/tmp/x.vim"), QStringLiteral("work.vim")})
        CHECK(rs.check({"nvim", "-S", bad}, "/", p).reason == NoRule);
    CHECK(rs.checkSaved(good, "/").trusted);
    CHECK(!rs.checkSaved({"nvim", "-S", sessions + "/sneaky.vim"}, "/").trusted);

    // The file: a list of [key, value]; a broken one trusts nothing.
    const QString file = home.path() + "/resume-rules.json";
    QFile f(file);
    f.open(QIODevice::WriteOnly);
    f.write(R"([["resume.trust.codex", false], ["resume.trust.claude.dir", "~/code"], "bad"])");
    f.close();
    const Rules loaded = load(file);
    CHECK(!loaded.isIncomplete && loaded.list.size() == 2 && loaded.list.first().dirs == QStringList{"~/code"});
    CHECK(loaded.diagnostics.size() == 1 && loaded.diagnostics.first().startsWith("resume-rules.json:#3"));
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("{ nope");
    f.close();
    CHECK(load(file).isIncomplete);
    CHECK(load(home.path() + "/none.json").list.size() == 3);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testParse();
    testRecords();
    testRules();
    testUnderAndFile();
    QTextStream(stdout) << (failures ? "FAILED: " : "ok: ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
