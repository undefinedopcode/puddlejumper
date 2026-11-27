/*
 * test-rexfilecopy — copying files with a Rex server's fs block.
 *
 * Without arguments: the tar written and read here, and hostile tars
 * refused. With a server (http://127.0.0.1:PORT or unix:///path), on this
 * machine: a tree copied up and back down again, patterns, refusals, and
 * names that are taken. The server's files are this machine's, so what
 * arrives is looked at directly.
 *
 *   ./build/test-rexfilecopy [server]
 */

#include "rexclient.h"
#include "rexfilecopy.h"
#include "rexsocket.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int g_checks = 0, g_failed = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", qPrintable(what));
    }
}

void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

// A tree, as "name type mode [content-or-link]" lines, sorted.
QStringList describe(const QString &root)
{
    QStringList out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString rel = QDir(root).relativeFilePath(p);
        struct stat sb;
        ::lstat(QFile::encodeName(p).constData(), &sb);
        const QString mode = QString::number(sb.st_mode & 0777, 8);
        if (S_ISLNK(sb.st_mode))
        {
            char buf[4096];
            const ssize_t n = ::readlink(QFile::encodeName(p).constData(), buf, sizeof buf);
            out << rel + QStringLiteral(" L ") + QString::fromUtf8(buf, n < 0 ? 0 : n);
        }
        else if (S_ISDIR(sb.st_mode))
            out << rel + QStringLiteral(" D ") + mode;
        else if (!S_ISREG(sb.st_mode))
            out << rel + QStringLiteral(" special");
        else
            out << rel + QStringLiteral(" F ") + mode + QLatin1Char(' ') + QString::fromUtf8(read(p).left(40))
                       + QLatin1Char(' ') + QString::number(qint64(sb.st_mtime));
    }
    out.sort();
    return out;
}

// The tar for local paths, whole, as RexFileCopy sends it.
QByteArray tarOf(const QStringList &paths)
{
    QList<rextar::Entry> entries;
    qint64 bytes = 0;
    int roots = 0;
    if (!rextar::collect(paths, &entries, &bytes, &roots).isEmpty())
        return {};
    QByteArray out;
    for (const rextar::Entry &e : entries) {
        out += rextar::header(e);
        if (e.type == rextar::Entry::File) {
            const QByteArray data = read(e.source);
            out += data + rextar::padding(data.size());
        }
    }
    return out + rextar::endMarker();
}

QByteArray entry(const QString &name, char type, const QByteArray &data = {}, const QString &link = {})
{
    rextar::Entry e;
    e.name = name;
    e.type = type == '5' ? rextar::Entry::Dir : type == '2' ? rextar::Entry::Link : rextar::Entry::File;
    e.size = data.size();
    e.link = link;
    e.mtime = 1600000000;
    QByteArray h = rextar::header(e);
    if (type == '1') {
        h[156] = '1';  // a hard link: patch the type, and the checksum
        unsigned sum = 0;
        memset(h.data() + 148, ' ', 8);
        for (int i = 0; i < 512; ++i)
            sum += uchar(h[i]);
        memcpy(h.data() + 148, QByteArray::number(sum, 8).rightJustified(6, '0').constData(), 6);
        h[154] = 0;
    }
    return h + data + rextar::padding(data.size());
}

bool unpacks(const QByteArray &tar, QString *error = nullptr, int chunk = 1000)
{
    QTemporaryDir stage;
    rextar::Reader r(stage.path());
    bool ok = true;
    for (qsizetype i = 0; ok && i < tar.size(); i += chunk)
        ok = r.feed(QByteArrayView(tar).sliced(i, std::min<qsizetype>(chunk, tar.size() - i)));
    ok = ok && r.finish();
    if (error)
        *error = r.error();
    return ok;
}

// A tree with what a copy has to get right: depth, a long name, an empty
// file and folder, a link, a FIFO (left out), permissions and a time.
void makeTree(const QString &src)
{
    write(src + QStringLiteral("/tree/a.txt"), "alpha");
    write(src + QStringLiteral("/tree/sub/b.txt"), QByteArray(70000, 'b'));
    write(src + QStringLiteral("/tree/empty.txt"), "");
    QDir().mkpath(src + QStringLiteral("/tree/emptydir"));
    const QString longDir = src + QStringLiteral("/tree/") + QString(60, QLatin1Char('d')) + QLatin1Char('/')
                            + QString(60, QLatin1Char('e'));
    write(longDir + QStringLiteral("/long name ünïcode.txt"), "long");
    ::symlink("sub/b.txt", QFile::encodeName(src + QStringLiteral("/tree/link")).constData());
    ::mkfifo(QFile::encodeName(src + QStringLiteral("/tree/fifo")).constData(), 0600);
    ::chmod(QFile::encodeName(src + QStringLiteral("/tree/a.txt")).constData(), 0640);
    ::chmod(QFile::encodeName(src + QStringLiteral("/tree/sub")).constData(), 0750);
    rextar::setMtime(src + QStringLiteral("/tree/a.txt"), 1500000000);
}

void offline(const QString &tmp)
{
    const QString src = tmp + QStringLiteral("/src");
    makeTree(src);

    const QByteArray tar = tarOf({src + QStringLiteral("/tree")});
    check(!tar.isEmpty(), QStringLiteral("collect"));
    for (int chunk : {1, 511, 512, 4096, 1 << 20}) {
        QTemporaryDir out;
        rextar::Reader r(out.path());
        bool ok = true;
        for (qsizetype i = 0; ok && i < tar.size(); i += chunk)
            ok = r.feed(QByteArrayView(tar).sliced(i, std::min<qsizetype>(chunk, tar.size() - i)));
        ok = ok && r.finish();
        check(ok, QStringLiteral("unpack in pieces of %1: %2").arg(chunk).arg(r.error()));
        QStringList want = describe(src);
        want.removeIf([](const QString &l) { return l.startsWith(QLatin1String("tree/fifo")); });
        check(describe(out.path()) == want,
              QStringLiteral("round trip in pieces of %1:\n  %2\nvs\n  %3")
                  .arg(chunk)
                  .arg(describe(out.path()).join(QLatin1String("\n  ")), want.join(QLatin1String("\n  "))));
        check(r.roots() == QStringList{QStringLiteral("tree")}, QStringLiteral("roots"));
    }

    const QByteArray end = rextar::endMarker();
    QString err;
    check(unpacks(entry(QStringLiteral("ok.txt"), '0', "x") + end), QStringLiteral("a plain file"));
    struct
    {
        const char *what;
        QByteArray tar;
    } refused[] = {
        {"a .. name", entry(QStringLiteral("../evil"), '0', "x") + end},
        {"an absolute name", entry(QStringLiteral("/evil"), '0', "x") + end},
        {"a hard link", entry(QStringLiteral("h"), '0', "x") + entry(QStringLiteral("h2"), '1', {}, QStringLiteral("h")) + end},
        {"a name twice", entry(QStringLiteral("t"), '0', "x") + entry(QStringLiteral("t"), '0', "x") + end},
        {"through a link", entry(QStringLiteral("l"), '5') + entry(QStringLiteral("l/s"), '2', {}, QStringLiteral("/tmp"))
                               + entry(QStringLiteral("l/s/x"), '0', "x") + end},
        {"before its directory", entry(QStringLiteral("nd/x"), '0', "x") + entry(QStringLiteral("nd"), '5') + end},
        {"no end marker", entry(QStringLiteral("p.txt"), '0', "x")},
        {"cut short in a file", entry(QStringLiteral("p.txt"), '0', "xyz").left(514)},
        {"garbage", QByteArray(1024, 'g')},
    };
    for (const auto &c : refused)
        check(!unpacks(c.tar, &err) && !err.isEmpty(), QStringLiteral("refused: %1").arg(QLatin1String(c.what)));
}

// ---- against a server -------------------------------------------------------

struct Result
{
    QString error;
    RexFileCopy *copy = nullptr;
};

bool wait(std::function<bool()> done, int ms = 30000)
{
    QEventLoop loop;
    QTimer tick;
    tick.start(20);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    if (!done())
        loop.exec();
    return done();
}

QString run(RexControl *control, const QString &sid, std::function<void(RexFileCopy *)> start,
            RexFileCopy **kept = nullptr)
{
    auto *copy = new RexFileCopy(control, sid);
    bool finished = false;
    QString error;
    QObject::connect(copy, &RexFileCopy::finished, [&](const QString &e) {
        finished = true;
        error = e;
    });
    start(copy);
    if (!wait([&]() { return finished; }, 60000))
        error = QStringLiteral("timed out");
    if (kept)
        *kept = copy;
    else
        copy->deleteLater();
    return error;
}

void live(const QUrl &server, const QString &tmp)
{
    RexControl control;
    control.open(server);
    if (!wait([&]() { return control.isReady(); }, 10000)) {
        check(false, QStringLiteral("connect: %1").arg(control.lastError()));
        return;
    }
    QString sid;
    bool got = false;
    control.request(QStringLiteral("session.create"),
                    QJsonObject{{QStringLiteral("label"), QStringLiteral("test-rexfilecopy")},
                                {QStringLiteral("initial_windows"),
                                 QJsonArray{QJsonObject{{QStringLiteral("layout"),
                                                         QJsonObject{{QStringLiteral("block"),
                                                                      QJsonObject{{QStringLiteral("flavor"),
                                                                                   QStringLiteral("com.superlogical.terminal.shell")}}}}}}}}},
                    [&](const QJsonObject &reply, const QJsonObject &) {
                        sid = reply.value(QLatin1String("session_id")).toString();
                        got = true;
                    });
    wait([&]() { return got; });
    check(!sid.isEmpty(), QStringLiteral("session.create"));
    if (sid.isEmpty())
        return;
    got = false;
    control.request(QStringLiteral("session.attach"), QJsonObject{{QStringLiteral("session_id"), sid}},
                    [&](const QJsonObject &, const QJsonObject &) { got = true; });
    wait([&]() { return got; });

    const QString src = tmp + QStringLiteral("/src"), up = tmp + QStringLiteral("/up"),
                  down = tmp + QStringLiteral("/down");
    QDir().mkpath(up);
    QDir().mkpath(down);
    makeTree(src);
    write(src + QStringLiteral("/one.txt"), "one");
    write(src + QStringLiteral("/big.bin"), QByteArray(5 << 20, 'z'));  // past the write queue's high water

    // Up: a tree and two files into a directory, relative to working_dir.
    RexFileCopy *copy = nullptr;
    QString e = run(&control, sid, [&](RexFileCopy *c) {
        c->upload({src + QStringLiteral("/tree"), src + QStringLiteral("/one.txt"), src + QStringLiteral("/big.bin")}, up, QString());
    }, &copy);
    check(e.isEmpty(), QStringLiteral("upload: %1").arg(e));
    check(copy && copy->destination() == up, QStringLiteral("upload destination %1").arg(copy ? copy->destination() : QString()));
    delete copy;
    QStringList want = describe(src);
    want.removeIf([](const QString &l) { return l.startsWith(QLatin1String("tree/fifo")); });
    check(describe(up) == want, QStringLiteral("uploaded tree:\n  %1").arg(describe(up).join(QLatin1String("\n  "))));
    check(QDir(up).entryList({QStringLiteral(".rex-upload*")}, QDir::Hidden | QDir::AllEntries).isEmpty(),
          QStringLiteral("no staging left on the server"));

    // One file onto a new name.
    e = run(&control, sid, [&](RexFileCopy *c) { c->upload({src + QStringLiteral("/one.txt")}, up, QStringLiteral("renamed.txt")); });
    check(e.isEmpty() && read(up + QStringLiteral("/renamed.txt")) == "one", QStringLiteral("upload onto a name: %1").arg(e));

    // A file where a directory is: refused, and nothing changed.
    write(tmp + QStringLiteral("/clash/tree"), "a file");
    e = run(&control, sid, [&](RexFileCopy *c) { c->upload({src + QStringLiteral("/tree")}, tmp + QStringLiteral("/clash"), QString()); });
    check(e.contains(QLatin1String("refused")), QStringLiteral("a directory onto a file: %1").arg(e));
    check(read(tmp + QStringLiteral("/clash/tree")) == "a file", QStringLiteral("the file left be"));

    // Down: the tree back, into an empty folder.
    e = run(&control, sid, [&](RexFileCopy *c) { c->download({QStringLiteral("tree"), QStringLiteral("big.bin")}, up, down); }, &copy);
    check(e.isEmpty(), QStringLiteral("download: %1").arg(e));
    check(copy && copy->placed().size() == 2, QStringLiteral("placed %1").arg(copy ? copy->placed().join(QLatin1Char(',')) : QString()));
    delete copy;
    QStringList wantDown = want;
    wantDown.removeIf([](const QString &l) { return l.startsWith(QLatin1String("one.txt")); });
    check(describe(down) == wantDown, QStringLiteral("downloaded tree:\n  %1").arg(describe(down).join(QLatin1String("\n  "))));
    // Again: nothing replaced, new names.
    e = run(&control, sid, [&](RexFileCopy *c) { c->download({up + QStringLiteral("/*.txt")}, QString(), down); }, &copy);
    check(e.isEmpty(), QStringLiteral("download a pattern: %1").arg(e));
    e = run(&control, sid, [&](RexFileCopy *c) { c->download({up + QStringLiteral("/one.txt")}, QString(), down); });
    check(e.isEmpty() && read(down + QStringLiteral("/one (2).txt")) == "one", QStringLiteral("a taken name: %1").arg(e));
    e = run(&control, sid, [&](RexFileCopy *c) { c->download({QStringLiteral("tree")}, up, down); });
    check(e.isEmpty() && QFileInfo(down + QStringLiteral("/tree (2)/sub/b.txt")).exists(), QStringLiteral("a taken folder name: %1").arg(e));
    delete copy;
    check(QDir(down).entryList({QStringLiteral(".puddlejumper-copy*")}, QDir::Hidden | QDir::AllEntries).isEmpty(),
          QStringLiteral("no staging left here"));

    // Refusals, as the server words them.
    e = run(&control, sid, [&](RexFileCopy *c) { c->download({QStringLiteral("zz*")}, up, down); });
    check(e.contains(QLatin1String("matches nothing")), QStringLiteral("nothing matches: %1").arg(e));
    e = run(&control, sid, [&](RexFileCopy *c) { c->upload({src + QStringLiteral("/one.txt")}, tmp + QStringLiteral("/nope"), QString()); });
    check(e.contains(QLatin1String("isn't a directory on the server")), QStringLiteral("a working dir that isn't: %1").arg(e));
    e = run(&control, sid, [&](RexFileCopy *c) { c->upload({tmp + QStringLiteral("/missing")}, up, QString()); });
    check(e.contains(QLatin1String("no such file")), QStringLiteral("a local file that isn't: %1").arg(e));

    // Cancelled mid-way: nothing left behind on either side.
    write(src + QStringLiteral("/huge.bin"), QByteArray(64 << 20, 'h'));
    {
        auto *c = new RexFileCopy(&control, sid);
        bool fin = false;
        QString err;
        QObject::connect(c, &RexFileCopy::finished, [&](const QString &x) { fin = true; err = x; });
        QObject::connect(c, &RexFileCopy::progress, c, [c](qint64 done, qint64) {
            if (done > (4 << 20))
                c->cancel();
        });
        c->upload({src + QStringLiteral("/huge.bin")}, up, QStringLiteral("cancelled.bin"));
        wait([&]() { return fin; });
        check(err == QStringLiteral("cancelled"), QStringLiteral("cancel: %1").arg(err));
        delete c;
        wait([&]() { return QDir(up).entryList({QStringLiteral(".rex-upload*")}, QDir::Hidden | QDir::AllEntries).isEmpty(); }, 5000);
        check(!QFileInfo::exists(up + QStringLiteral("/cancelled.bin")), QStringLiteral("nothing arrived after a cancel"));
    }

    // The session's blocks are only its shell: every copy closed its block.
    QJsonObject view;
    got = false;
    control.request(QStringLiteral("session.destroy"), QJsonObject{{QStringLiteral("session_id"), sid}},
                    [&](const QJsonObject &, const QJsonObject &) { got = true; });
    wait([&]() { return got; });
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    offline(tmp.path());
    if (argc > 1) {
        QTemporaryDir live_(QDir::tempPath() + QStringLiteral("/pj-copy-XXXXXX"));
        live(rexaddress::parse(QString::fromLocal8Bit(argv[1])), live_.path());
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
