/*
 * test-localresume — program resume (OSC 7577) in real local panes: a
 * program signs how to start it again with its pane's token, the rules
 * decide, and a new pane starts it again through the helper. Runs with its
 * own XDG config and data directories (a temporary one), so the real
 * settings and rules are left alone. Prints each failure; exits 1 if any.
 *
 * With PJ_RESUME_TEST_COMMAND set, also types that into a pane (say, a
 * stand-in claude that runs a SessionStart hook) and expects a record the
 * built-in rules trust; PJ_RESUME_TEST_EXPECT is then the program it should
 * be resumed with (a stub on PATH), checked by the file it writes,
 * PJ_RESUME_TEST_EXPECT_FILE.
 */

#include "programresume.h"
#include "resumerules.h"
#include "settings.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstring>
#include <functional>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    QTextStream(ok ? stdout : stderr) << (ok ? "ok    " : "FAIL  ") << what << "\n";
    if (!ok)
        ++failures;
}

bool waitFor(const std::function<bool()> &done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (done())
            return true;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        ::usleep(20000);
    }
    return done();
}

TerminalWidget *newPane(int id, const Settings &settings)
{
    TerminalProfile sh;   // a plain shell: quick to start, no rc files
    sh.shellProgram = QStringLiteral("/bin/sh");
    auto *pane = new TerminalWidget(id, settings, sh);
    pane->resize(700, 300);
    pane->show();
    waitFor([pane] { return !pane->foregroundProgramName().isEmpty(); }, 5000);
    waitFor([] { return false; }, 300);   // its prompt
    return pane;
}

} // namespace

int main(int argc, char *argv[])
{
    // The panes' shells run this binary as the helper, as they would the app.
    if (argc == 3 && std::strcmp(argv[1], "--resume-helper") == 0)
        return ProgramResume::runHelper(argv[2]);

    QTemporaryDir home;
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toLocal8Bit());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("OSC7577_TOKEN");
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("puddlejumper-test"));
    app.setApplicationName(QStringLiteral("puddlejumper-test"));

    // A program that reports for itself and keeps running (one that went on
    // to exec another program couldn't be trusted).
    const QString dir = QDir(home.path()).canonicalPath();
    const QString script = dir + QStringLiteral("/resumer");
    {
        QFile f(script);
        f.open(QIODevice::WriteOnly);
        f.write(R"SH(#!/bin/sh
b() { printf '%s' "$1" | base64 | tr -d '\n'; }
body="pid=$$:time=$(date +%s):argv=$(b "$0"),$(b "$1"):cwd=$(b "$PWD")"
mac=$(printf '%s' "$body:$OSC7577_TOKEN" | sha256sum | cut -c1-64)
printf '\033]7577;%s:mac=%s\033\\' "$body" "$mac"
echo "resumer $1 running" > "$0.$1.ran"
sleep 30
)SH");
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    // A rule for it, in the rules file.
    QDir().mkpath(home.path() + QStringLiteral("/config/puddlejumper"));
    {
        QFile f(resumerules::path());
        check(f.fileName().startsWith(home.path()), QStringLiteral("the rules file is the test's (%1)").arg(f.fileName()));
        f.open(QIODevice::WriteOnly);
        f.write(QStringLiteral(R"([["resume.trust.resumer", {"command": ["%1", ["one", "two"]]}]])").arg(script).toUtf8());
    }
    resumerules::reload();
    check(resumerules::current().list.size() == 4, QStringLiteral("built-in rules and the test's"));

    Settings settings;

    // ---- A trusted program: its record, and the command as the rule has it.
    TerminalWidget *a = newPane(1, settings);
    a->sendText(QStringLiteral("cd '%1' && '%2' one\r").arg(dir, script));
    QString cwd;
    resumerules::Verdict v;
    waitFor([&] { v = a->resumeVerdict(&cwd); return v.trusted; }, 6000);
    check(v.trusted && v.rule == QLatin1String("resumer"), QStringLiteral("trusted by its rule (%1 %2)").arg(v.rule, v.reason));
    check(v.trusted && v.command == QStringList({script, QStringLiteral("one")}) && cwd == dir,
          QStringLiteral("the command and directory (%1 in %2; %3)").arg(v.command.join(' '), cwd, v.reason));

    // ---- One no rule names.
    TerminalWidget *b = newPane(2, settings);
    b->sendText(QStringLiteral("'%1' three\r").arg(script));
    resumerules::Verdict untrusted;
    waitFor([&] { untrusted = b->resumeVerdict(); return !untrusted.reason.isEmpty(); }, 6000);
    check(!untrusted.trusted && untrusted.reason == resumerules::NoRule,
          QStringLiteral("untrusted: %1").arg(untrusted.reason));

    // ---- A restore: a new pane starts it again through the helper, and
    // the program reports again, with the new pane's token.
    QFile::remove(script + QStringLiteral(".one.ran"));
    TerminalWidget *c = newPane(3, settings);
    c->resumeProgram(v.command, cwd);
    resumerules::Verdict again;
    waitFor([&] { again = c->resumeVerdict(); return again.trusted; }, 8000);
    check(again.trusted && again.command == v.command, QStringLiteral("resumed in the new pane (%1)").arg(again.reason));
    check(QFile::exists(script + QStringLiteral(".one.ran")), QStringLiteral("the program ran again"));
    check(QDir(TerminalWidget::resumeDirectory()).entryList(QDir::Files).isEmpty(),
          QStringLiteral("the helper removed its file"));

    // ---- The rules in force at the restore decide again.
    {
        QFile f(resumerules::path());
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(R"([["resume.trust.resumer", false]])");
    }
    resumerules::reload();
    check(resumerules::current().checkSaved(v.command, cwd).reason == resumerules::NoRule,
          QStringLiteral("a rule removed since: not resumed"));

    // ---- Optionally, a real hook under a stand-in claude.
    if (const QString command = qEnvironmentVariable("PJ_RESUME_TEST_COMMAND"); !command.isEmpty()) {
        QFile::remove(resumerules::path());
        resumerules::reload();
        TerminalWidget *d = newPane(4, settings);
        d->sendText(command + QLatin1Char('\r'));
        resumerules::Verdict hook;
        QString hookCwd;
        waitFor([&] { hook = d->resumeVerdict(&hookCwd); return hook.trusted; }, 8000);
        check(hook.trusted && hook.rule == QLatin1String("claude"),
              QStringLiteral("the hook's report: %1 (%2) in %3").arg(hook.command.join(' '), hook.reason, hookCwd));
        const QString expectFile = qEnvironmentVariable("PJ_RESUME_TEST_EXPECT_FILE");
        if (hook.trusted && !expectFile.isEmpty()) {
            QFile::remove(expectFile);
            TerminalWidget *e = newPane(5, settings);
            e->resumeProgram(hook.command, hookCwd);
            waitFor([&] { return QFile::exists(expectFile); }, 8000);
            QFile f(expectFile);
            f.open(QIODevice::ReadOnly);
            const QString got = QString::fromUtf8(f.readAll()).trimmed();
            check(got == qEnvironmentVariable("PJ_RESUME_TEST_EXPECT"), QStringLiteral("resumed as: %1").arg(got));
        }
    }

    QTextStream(stdout) << (failures ? "FAILED: " : "ok: ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
