/*
 * test-programstatus — the OSC 7501 parser and records against the rules of
 * the Program Status Protocol spec (rev. 0.4). Prints each failure; exits 1
 * if there was any.
 */

#include "programstatus.h"

#include <QCoreApplication>
#include <QTextStream>

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

QString b64(const QString &text)
{
    return QString::fromLatin1(text.toUtf8().toBase64());
}

std::optional<ProgramStatus::Report> parse(const QString &body)
{
    return ProgramStatus::parse(body);
}

QStringList ids(const ProgramStatus &s)
{
    QStringList out;
    for (const ProgramStatusRecord &r : s.records())
        out << (r.id.isEmpty() ? QStringLiteral("<root>") : r.id);
    out.sort();
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // The spec's example.
    {
        const QString msg = QStringLiteral("Apply 3 to add, 1 to change, 0 to destroy?");
        const auto r = parse(QStringLiteral("state=blocked:kind=permission:app=terraform:msg=") + b64(msg));
        CHECK(r && r->record.state == ProgramStatusRecord::Blocked);
        CHECK(r && r->record.kind == QLatin1String("permission"));
        CHECK(r && r->record.app == QLatin1String("terraform"));
        CHECK(r && r->record.msg == msg);
        CHECK(r && r->record.id.isEmpty());
    }

    // Base64: padding optional; bad base64, control characters and
    // over-long text discard the whole report.
    CHECK(parse(QStringLiteral("state=done:msg=aGk")) && parse(QStringLiteral("state=done:msg=aGk"))->record.msg == QLatin1String("hi"));
    CHECK(parse(QStringLiteral("state=done:msg=aGk=")));
    CHECK(!parse(QStringLiteral("state=done:msg=a")));
    CHECK(!parse(QStringLiteral("state=done:msg=") + b64(QStringLiteral("a\nb"))));
    CHECK(!parse(QStringLiteral("state=done:msg=") + b64(QStringLiteral("a\tb"))));
    CHECK(!parse(QStringLiteral("state=done:msg=") + b64(QString(QChar(0x85)))));
    CHECK(!parse(QStringLiteral("state=done:msg=") + b64(QString(2049, QLatin1Char('x')))));
    CHECK(parse(QStringLiteral("state=done:msg=") + b64(QString(2048, QLatin1Char('x')))));
    CHECK(!parse(QStringLiteral("state=done:title=") + b64(QString(193, QLatin1Char('x')))));
    CHECK(!parse(QStringLiteral("state=done:msg=") + QString::fromLatin1(QByteArray("\xff\xfe").toBase64())));
    // Invisible formatting is dropped, not refused.
    {
        const auto r = parse(QStringLiteral("state=done:msg=") + b64(QStringLiteral("a") + QChar(0x202E) + QStringLiteral("b") + QChar(0x200B) + QStringLiteral("c")));
        CHECK(r && r->record.msg == QLatin1String("abc"));
    }

    // state: required, and one this doesn't know is ignored.
    CHECK(!parse(QStringLiteral("app=cargo")));
    CHECK(!parse(QStringLiteral("state=sleeping")));
    CHECK(!parse(QString()));

    // Pairs: unknown keys ignored, last wins, whitespace stripped, a
    // malformed pair skipped but the rest kept.
    {
        const auto r = parse(QStringLiteral(" state = working : future=1 : app=a : app=b : junk : =x : Up=1 : bad=a!b"));
        CHECK(r && r->record.state == ProgramStatusRecord::Working);
        CHECK(r && r->record.app == QLatin1String("b"));
    }
    CHECK(!parse(QStringLiteral("state=done:abcdefghijklmnopq=1")));   // key over 16 bytes
    CHECK(parse(QStringLiteral("state=done:abcdefghijklmnop=1")));
    CHECK(!parse(QStringLiteral("state=done:x=") + QString(4090, QLatin1Char('a'))));   // over 4096

    // ids: a bad one is never the root.
    CHECK(parse(QStringLiteral("state=working:id=build/test-1.x")));
    CHECK(!parse(QStringLiteral("state=working:id=")));
    CHECK(!parse(QStringLiteral("state=working:id=a//b")));
    CHECK(!parse(QStringLiteral("state=working:id=a/")));
    CHECK(!parse(QStringLiteral("state=working:id=") + QString(33, QLatin1Char('a'))));
    CHECK(parse(QStringLiteral("state=working:id=a/b/c/d/e/f/g/h")));
    CHECK(!parse(QStringLiteral("state=working:id=a/b/c/d/e/f/g/h/i")));
    CHECK(!parse(QStringLiteral("state=working:id=a,b")));

    // progress, kind and app: what isn't valid, or isn't for this state, is
    // absent.
    CHECK(parse(QStringLiteral("state=working:progress=40"))->record.progress == 40);
    CHECK(parse(QStringLiteral("state=working:progress=0"))->record.progress == 0);
    CHECK(parse(QStringLiteral("state=working:progress=100"))->record.progress == 100);
    CHECK(parse(QStringLiteral("state=working:progress=101"))->record.progress == -1);
    CHECK(parse(QStringLiteral("state=working:progress=040"))->record.progress == -1);
    CHECK(parse(QStringLiteral("state=working:progress=-1"))->record.progress == -1);
    CHECK(parse(QStringLiteral("state=blocked:progress=5"))->record.progress == 5);
    CHECK(parse(QStringLiteral("state=done:progress=5"))->record.progress == -1);
    CHECK(parse(QStringLiteral("state=working:kind=auth"))->record.kind.isEmpty());
    CHECK(parse(QStringLiteral("state=blocked:kind=coffee"))->record.kind.isEmpty());
    CHECK(parse(QStringLiteral("state=done:app=") + QString(33, QLatin1Char('a')))->record.app.isEmpty());

    // Records: a report replaces its record whole.
    {
        ProgramStatus s;
        CHECK(s.apply(QStringLiteral("state=working:app=cargo:progress=10")));
        CHECK(s.apply(QStringLiteral("state=working:progress=20")));
        CHECK(s.records().size() == 1);
        CHECK(s.records().first().app.isEmpty());
        CHECK(s.records().first().progress == 20);
        CHECK(!s.apply(QStringLiteral("state=nonsense")));
        CHECK(s.records().size() == 1);
    }

    // clear: the record and those below it; with no id, everything.
    {
        ProgramStatus s;
        for (const char *id : {"build", "build/test", "build/test/unit", "buildx", "deploy"})
            s.apply(QStringLiteral("state=working:id=") + QLatin1String(id));
        s.apply(QStringLiteral("state=idle"));
        s.apply(QStringLiteral("state=clear:id=build"));
        CHECK(ids(s) == QStringList({QStringLiteral("<root>"), QStringLiteral("buildx"), QStringLiteral("deploy")}));
        s.apply(QStringLiteral("state=clear"));
        CHECK(s.isEmpty());
    }

    // app comes from the nearest ancestor that has one.
    {
        ProgramStatus s;
        s.apply(QStringLiteral("state=working:app=deploy"));
        s.apply(QStringLiteral("state=working:id=us/east"));
        s.apply(QStringLiteral("state=working:id=eu:app=other"));
        s.apply(QStringLiteral("state=blocked:id=eu/west"));
        for (const ProgramStatusRecord &r : s.records()) {
            if (r.id == QLatin1String("us/east"))
                CHECK(s.app(r) == QLatin1String("deploy"));
            if (r.id == QLatin1String("eu/west"))
                CHECK(s.app(r) == QLatin1String("other"));
        }
        // The most pressing record carries its inherited app.
        CHECK(s.mostPressing().state == ProgramStatusRecord::Blocked);
        CHECK(s.mostPressing().app == QLatin1String("other"));
    }

    // Lifetime: working, blocked and idle go when the program does; done and
    // error stay, and stop being pressing once seen. Reset removes all.
    {
        ProgramStatus s;
        s.apply(QStringLiteral("state=working:id=a"));
        s.apply(QStringLiteral("state=blocked:id=b"));
        s.apply(QStringLiteral("state=idle:id=c"));
        s.apply(QStringLiteral("state=done:id=d"));
        s.apply(QStringLiteral("state=error:id=e"));
        CHECK(s.mostPressing().id == QLatin1String("b"));
        s.programEnded();
        CHECK(ids(s) == QStringList({QStringLiteral("d"), QStringLiteral("e")}));
        CHECK(s.mostPressing().id == QLatin1String("e"));
        s.markSeen();
        CHECK(s.mostPressing().state == ProgramStatusRecord::None);
        CHECK(s.records().size() == 2);
        s.apply(QStringLiteral("state=done:id=f"));
        CHECK(s.mostPressing().id == QLatin1String("f"));
        s.reset();
        CHECK(s.isEmpty());
    }

    // Idle is nothing to show; of two of a kind, the later.
    {
        ProgramStatus s;
        s.apply(QStringLiteral("state=idle"));
        CHECK(s.mostPressing().state == ProgramStatusRecord::None);
        s.apply(QStringLiteral("state=working:id=x"));
        s.apply(QStringLiteral("state=working:id=y"));
        CHECK(s.mostPressing().id == QLatin1String("y"));
        s.apply(QStringLiteral("state=working:id=x:progress=3"));
        CHECK(s.mostPressing().id == QLatin1String("x"));
    }

    // OSC 9;4 progress is the root record, until a program reports with
    // OSC 7501; a full reset lets it in again.
    {
        ProgramStatus s;
        CHECK(s.applyProgress(1, 30));
        CHECK(s.mostPressing().state == ProgramStatusRecord::Working && s.mostPressing().progress == 30);
        CHECK(s.applyProgress(3, 50));
        CHECK(s.mostPressing().progress == -1);
        CHECK(s.applyProgress(2, 0));
        CHECK(s.mostPressing().state == ProgramStatusRecord::Error);
        CHECK(s.applyProgress(0, 0));
        CHECK(s.isEmpty());
        CHECK(!s.applyProgress(9, 0));
        CHECK(s.applyProgress(1, 101) && s.mostPressing().progress == -1);
        s.apply(QStringLiteral("state=blocked:kind=question:id=x"));
        CHECK(!s.applyProgress(1, 10));
        s.reset();
        CHECK(s.applyProgress(1, 10));
    }

    // At most 256 records: the least recently updated makes way.
    {
        ProgramStatus s;
        for (int i = 0; i < 257; ++i)
            s.apply(QStringLiteral("state=working:id=r%1").arg(i));
        CHECK(s.records().size() == 256);
        CHECK(!ids(s).contains(QLatin1String("r0")));
        CHECK(ids(s).contains(QLatin1String("r256")));
    }

    QTextStream(stdout) << (failures ? "FAILED: " : "ok") << (failures ? QString::number(failures) : QString()) << "\n";
    return failures ? 1 : 0;
}
