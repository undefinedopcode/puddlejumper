/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexterminalstream: RexTerminalStream on a stream captured from a
 * Rex-compatible server (dev/rex-capture), cut into pieces every which way,
 * and the whole client path (snapshot, replay, output, resizes) into
 * qtermwidget's emulator against the server's own text of the terminal.
 * Run: ./build/test-rexterminalstream [capture dir]
 */

#include "ghosttyreplay.h"
#include "qtermprobe.h"
#include "rexterminalstream.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTextStream>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL: " << what << "\n";
        ++failures;
    }
}

QByteArray readFile(const QString &dir, const QString &name)
{
    QFile f(QDir(dir).filePath(name));
    if (!f.open(QIODevice::ReadOnly)) {
        check(false, QStringLiteral("read ") + f.fileName());
        return {};
    }
    return f.readAll();
}

// The stream's events with output run together, as a list of strings to
// compare: "snapshot", "marker <json>", "output <n> bytes" (and the bytes).
struct Taken
{
    int snapshots = 0;
    QList<QJsonObject> markers;
    QByteArray output;
    QStringList order;  // snapshot / marker type / output, output merged
};

void take(Taken &t, const QList<RexTerminalStream::Event> &events)
{
    for (const auto &e : events) {
        switch (e.kind) {
        case RexTerminalStream::Event::Snapshot:
            ++t.snapshots;
            t.order << QStringLiteral("snapshot");
            break;
        case RexTerminalStream::Event::Marker:
            t.markers << e.marker;
            t.order << QStringLiteral("marker ") + e.type();
            break;
        case RexTerminalStream::Event::Output:
            t.output += e.bytes;
            if (t.order.isEmpty() || t.order.last() != QLatin1String("output"))
                t.order << QStringLiteral("output");
            break;
        }
    }
}

Taken feedInPieces(quint64 nonce, const QByteArray &stream, const QList<qsizetype> &cuts, quint64 *received = nullptr)
{
    RexTerminalStream s(nonce);
    Taken t;
    qsizetype pos = 0;
    for (qsizetype cut : cuts) {
        take(t, s.feed(QByteArrayView(stream).sliced(pos, cut - pos)));
        pos = cut;
    }
    take(t, s.feed(QByteArrayView(stream).sliced(pos)));
    check(!s.failed(), QStringLiteral("no error: ") + s.error());
    if (received)
        *received = s.received();
    return t;
}

void testWelcome(const QByteArray &welcome, quint64 *nonce)
{
    quint32 grace = 0;
    QByteArray token;
    check(RexTerminalStream::parseWelcome(welcome, nonce, &grace, &token), QStringLiteral("a welcome"));
    check(grace == 60000, QStringLiteral("grace %1").arg(grace));
    check(token.size() == 16, QStringLiteral("token"));
    check(*nonce != 0, QStringLiteral("nonce"));
    quint64 n = 1;
    check(!RexTerminalStream::parseWelcome(welcome.left(31), &n) && n == 0, QStringLiteral("short welcome refused"));
    QByteArray reject = welcome;
    reject[0] = char(0x82);
    check(!RexTerminalStream::parseWelcome(reject, &n), QStringLiteral("not a welcome"));
}

void testPieces(quint64 nonce, const QByteArray &stream)
{
    quint64 received = 0;
    const Taken whole = feedInPieces(nonce, stream, {}, &received);
    check(received == quint64(stream.size()), QStringLiteral("received counts every byte"));
    check(whole.snapshots == 1, QStringLiteral("one snapshot"));
    // As the server sends them: snapshot, a resize at once, then output with
    // a resize to 70x20 and one back to 80x24.
    check(whole.order == QStringList({QStringLiteral("snapshot"), QStringLiteral("marker resize"),
                                      QStringLiteral("output"), QStringLiteral("marker resize"),
                                      QStringLiteral("output"), QStringLiteral("marker resize"),
                                      QStringLiteral("output")}),
          QStringLiteral("order: ") + whole.order.join(QStringLiteral(", ")));
    QList<QPair<int, int>> sizes;
    for (const QJsonObject &m : whole.markers)
        sizes << qMakePair(m.value(QLatin1String("cols")).toInt(), m.value(QLatin1String("rows")).toInt());
    check(sizes == QList<QPair<int, int>>({{80, 24}, {70, 20}, {80, 24}}), QStringLiteral("resize sizes"));
    check(!whole.output.contains("rex1;"), QStringLiteral("no marker left in the output"));
    check(whole.output.contains("live line one") && whole.output.endsWith("sh-5.3$ "), QStringLiteral("output"));

    // Every split in two, then random splits and a byte at a time: the same.
    for (qsizetype cut = 1; cut < stream.size(); ++cut) {
        const Taken t = feedInPieces(nonce, stream, {cut});
        if (t.order != whole.order || t.output != whole.output || t.markers != whole.markers) {
            check(false, QStringLiteral("split at %1").arg(cut));
            break;
        }
    }
    QRandomGenerator rng(7);
    for (int round = 0; round < 200; ++round) {
        QList<qsizetype> cuts;
        for (qsizetype pos = 0;;) {
            pos += 1 + rng.bounded(round < 100 ? 64 : 700);
            if (pos >= stream.size())
                break;
            cuts << pos;
        }
        const Taken t = feedInPieces(nonce, stream, cuts);
        if (t.order != whole.order || t.output != whole.output || t.markers != whole.markers) {
            check(false, QStringLiteral("random pieces, round %1").arg(round));
            break;
        }
    }
    QList<qsizetype> ones;
    for (qsizetype i = 1; i < stream.size(); ++i)
        ones << i;
    const Taken bytewise = feedInPieces(nonce, stream, ones);
    check(bytewise.order == whole.order && bytewise.output == whole.output, QStringLiteral("a byte at a time"));

    // Another nonce: its markers are output like any other APC.
    const Taken other = feedInPieces(nonce + 1, stream, {});
    check(other.markers.isEmpty() && other.output.contains("rex1;"), QStringLiteral("other nonce's markers left alone"));
}

void testSynthetic()
{
    const quint64 nonce = 42;
    // A resumed stream: no snapshot, markers and output only.
    {
        RexTerminalStream s(nonce, true);
        Taken t;
        take(t, s.feed("ab\033_rex1;42;{\"type\":\"theme\",\"foreground\":\"#ffffff\"}\033\\cd"));
        take(t, s.feed("\033_rex1;42;{\"type\":\"exit\",\"exit_code\":7}\033\\"));
        check(t.output == "abcd", QStringLiteral("resumed output"));
        check(t.markers.size() == 2 && t.markers[1].value(QLatin1String("exit_code")).toInt() == 7,
              QStringLiteral("resumed markers"));
    }
    // Something that starts like a marker and isn't: output, once it's clear.
    {
        RexTerminalStream s(nonce, true);
        Taken t;
        take(t, s.feed("x\033_rex1;4"));
        check(t.output == "x", QStringLiteral("a possible marker is held back"));
        take(t, s.feed("3;{}\033\\y"));
        check(t.output == "x\033_rex1;43;{}\033\\y" && t.markers.isEmpty(), QStringLiteral("not ours: output"));
    }
    // A marker that never ends gives its bytes back as output.
    {
        RexTerminalStream s(nonce, true);
        Taken t;
        take(t, s.feed("\033_rex1;42;{"));
        take(t, s.feed(QByteArray(RexTerminalStream::kMaxMarker, 'z')));
        check(t.markers.isEmpty() && t.output.startsWith("\033_rex1;42;{zzz"), QStringLiteral("unended marker"));
    }
    // Bad JSON: a marker of type "invalid", the stream goes on.
    {
        RexTerminalStream s(nonce, true);
        Taken t;
        take(t, s.feed("\033_rex1;42;not json\033\\ok"));
        check(t.markers.size() == 1 && t.markers[0].value(QLatin1String("type")).toString() == QLatin1String("invalid")
                  && t.output == "ok",
              QStringLiteral("invalid marker"));
    }
    // Not a snapshot at all.
    {
        RexTerminalStream s(nonce);
        s.feed("hello, this is not GHOSTSNP");
        check(s.failed(), QStringLiteral("no snapshot: failed"));
    }
}

// The client's whole path, against the server's own text of the terminal.
void testEndToEnd(quint64 nonce, const QByteArray &stream, const QByteArray &serverText)
{
    RexTerminalStream s(nonce);
    std::unique_ptr<QtermProbe> emu;
    for (const auto &e : s.feed(stream)) {
        if (e.kind == RexTerminalStream::Event::Snapshot) {
            emu = std::make_unique<QtermProbe>(e.snapshot->terminal.columns, e.snapshot->terminal.rows);
            emu->feed(GhosttySnapshot::replay(*e.snapshot));
        } else if (!emu) {
            check(false, QStringLiteral("output before the snapshot"));
            return;
        } else if (e.kind == RexTerminalStream::Event::Output) {
            emu->feed(e.bytes);
        } else if (e.type() == QLatin1String("resize")) {
            const int cols = e.marker.value(QLatin1String("cols")).toInt();
            const int rows = e.marker.value(QLatin1String("rows")).toInt();
            if (cols > 0 && rows > 0)
                emu->setImageSize(rows, cols);
        }
    }
    if (!emu)
        return;
    QStringList got = QtermProbeLines::screenLines(emu->screen(0));
    while (!got.isEmpty() && got.last().isEmpty())
        got.removeLast();
    QStringList want = QString::fromUtf8(serverText).split(QLatin1Char('\n'));
    while (!want.isEmpty() && want.last().isEmpty())
        want.removeLast();
    const bool same = got == want;
    if (!same) {
        QTextStream err(stderr);
        err << "    client:\n";
        for (const QString &l : got)
            err << "      |" << l << "|\n";
        err << "    server:\n";
        for (const QString &l : want)
            err << "      |" << l << "|\n";
    }
    check(same, QStringLiteral("the client's terminal matches the server's"));
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(PJ_REX_CAPTURES);

    const QByteArray welcome = readFile(dir, QStringLiteral("shell-welcome.bin"));
    const QByteArray stream = readFile(dir, QStringLiteral("shell-stream.bin"));
    const QByteArray text = readFile(dir, QStringLiteral("shell-screen.txt"));
    quint64 nonce = 0;
    testWelcome(welcome, &nonce);
    testPieces(nonce, stream);
    testSynthetic();
    testEndToEnd(nonce, stream, text);

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
