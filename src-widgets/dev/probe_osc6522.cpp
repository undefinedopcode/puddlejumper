/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * probe-osc6522: how large an OSC sequence gets through a Rex-compatible
 * server unchanged, in both directions. Route A of "puddle as Rex blocks"
 * sends widget trees as OSC 6522 in the program's output and the client's
 * messages as OSC 6522 in the pane's input, so both have to arrive whole.
 *
 * A block runs dev/osc6522-helper.py, then:
 *
 *   output  the helper writes sequences of each size, ended with ST and with
 *           BEL; each must be in the stream this client gets (the bytes
 *           RexTerminalConnection passes on), byte for byte.
 *   screen  neither the server's terminal nor qtermwidget's emulator shows
 *           any of it as text.
 *   input   sequences of each size go in with com.superlogical.terminal.write
 *           in 32 KiB pieces, as RexSession::sendInput sends them; the helper
 *           reports the length and hash of what it got.
 *
 * It creates one session and destroys it. A report, not a pass/fail test:
 * it prints a table and exits 0 unless it couldn't run.
 *   ./build/probe-osc6522 unix:///run/user/1000/rex/server.sock
 */

#include "rexprobe.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>

using namespace rexprobe;

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

// The helper's data for a sequence of n bytes (see osc6522-helper.py).
QByteArray pattern(int n)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    QByteArray b(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i)
        b[i] = alphabet[(qint64(i) * 7 + n) % 64];
    return b;
}

QByteArray body(int n) { return "op=probe:n=" + QByteArray::number(n) + ":data=" + pattern(n); }

QByteArray sequence(int n, bool st) { return "\x1b]6522;" + body(n) + (st ? QByteArray("\x1b\\") : QByteArray("\x07")); }

QString sizeText(qint64 n)
{
    if (n >= 1024 * 1024)
        return QString::number(double(n) / (1024 * 1024), 'f', 2) + QStringLiteral(" MiB");
    if (n >= 1024)
        return QString::number(double(n) / 1024, 'f', 1) + QStringLiteral(" KiB");
    return QString::number(n) + QStringLiteral(" B");
}

// What the stream carried for one sequence.
QString verdict(const QByteArray &got, int n, bool st)
{
    const QByteArray want = sequence(n, st);
    if (got.contains(want))
        return QStringLiteral("whole");
    const qsizetype at = got.indexOf("\x1b]6522;op=probe:n=" + QByteArray::number(n) + ":");
    if (at < 0)
        return got.contains("\x1b]6522;") ? QStringLiteral("altered opener") : QStringLiteral("missing");
    // How far it matches before it differs or stops.
    qsizetype same = 0;
    while (same < want.size() && at + same < got.size() && got[at + same] == want[same])
        ++same;
    if (at + same >= got.size())
        return QStringLiteral("cut after %1").arg(sizeText(same));
    return QStringLiteral("differs at byte %1").arg(same);
}

// Anything of a sequence that shows as text: its keys or a run of its data.
bool leaked(const QString &text)
{
    static const QRegularExpression run(QStringLiteral("[A-Za-z0-9+/]{24,}"));
    return text.contains(QLatin1String("op=probe")) || text.contains(QLatin1String("6522")) || run.match(text).hasMatch();
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString server = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("PJ_REX_SERVER");
    if (server.isEmpty()) {
        out << "usage: probe-osc6522 unix:///path | http://host:port (or PJ_REX_SERVER)\n";
        return 2;
    }
    const QString helper = QStringLiteral(PJ_OSC6522_HELPER);
    if (!QFileInfo::exists(helper)) {
        err << "no helper at " << helper << "\n";
        return 2;
    }

    Block b;
    if (!b.open(server, {QStringLiteral("python3"), QStringLiteral("-u"), helper}, QStringLiteral("probe-osc6522"))) {
        err << b.error() << "\n";
        return 2;
    }
    out << "server: " << server << "\n";
    // READY may be in the snapshot rather than the output.
    waitFor([&] { return b.stream.contains("READY"); }, 3000);

    const QList<int> sizes{100, 2000, 4000, 4200, 8000, 16000, 32000, 65000, 66000, 131000, 262000, 1048000};

    // Output: program → client.
    out << "\noutput (program writes, client stream receives)\n";
    out << QStringLiteral("%1  %2  %3  %4  %5\n").arg(QStringLiteral("sequence"), -10).arg(QStringLiteral("end"), -4)
               .arg(QStringLiteral("result"), -24).arg(QStringLiteral("output events"), -13).arg(QStringLiteral("ms"));
    bool anyLeak = false;
    for (int n : sizes) {
        for (bool st : {true, false}) {
            const qsizetype mark = b.stream.size();
            const int eventsBefore = b.outputEvents;
            const QByteArray done = "EMITTED " + QByteArray::number(n) + (st ? " st" : " bel");
            QElapsedTimer t;
            t.start();
            if (!b.write("E " + QByteArray::number(n) + (st ? " st" : " bel") + "\r")) {
                out << "  write failed: " << b.writeError << "\n";
                b.close();
                return 1;
            }
            const bool arrived = waitFor([&] { return b.stream.indexOf(done, mark) >= 0; }, 30000);
            const QString result = arrived ? verdict(b.stream.mid(mark), n, st) : QStringLiteral("no EMITTED (timeout)");
            out << QStringLiteral("%1  %2  %3  %4  %5\n")
                       .arg(sizeText(sequence(n, st).size()), -10)
                       .arg(st ? QStringLiteral("ST") : QStringLiteral("BEL"), -4)
                       .arg(result, -24)
                       .arg(b.outputEvents - eventsBefore, -13)
                       .arg(t.elapsed());
            out.flush();
            if (!b.failure.isEmpty()) {
                out << "  stream failed: " << b.failure << "\n";
                b.close();
                return 1;
            }
        }
        // After each size, so a leak can be placed.
        const bool onServer = leaked(b.serverScreen()), onClient = leaked(b.clientScreen());
        if (onServer || onClient) {
            anyLeak = true;
            out << "  text on screen after " << sizeText(n) << " (server: " << (onServer ? "yes" : "no")
                << ", client emulator: " << (onClient ? "yes" : "no") << ")\n";
        }
    }
    out << "screen: " << (anyLeak ? "sequence text showed (see above)" : "nothing of the sequences showed, server or client") << "\n";

    // A fresh stream: the snapshot is the terminal's state, not its output.
    if (b.reopen())
        out << "fresh snapshot after the sequences: opened\n";

    // Input: client → program.
    out << "\ninput (client writes as pane input, program receives)\n";
    out << QStringLiteral("%1  %2  %3\n").arg(QStringLiteral("sequence"), -10).arg(QStringLiteral("end"), -4).arg(QStringLiteral("result"));
    for (int n : sizes) {
        for (bool st : {true, false}) {
            const qsizetype mark = b.stream.size();
            const QByteArray bytes = body(n);
            const QByteArray want = "GOT " + QByteArray::number(bytes.size()) + " "
                + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(16);
            QString result;
            if (!b.write(sequence(n, st))) {
                result = QStringLiteral("write refused: ") + b.writeError;
            } else if (const auto got = b.lineAfter(mark, "GOT ", 30000)) {
                result = *got == want ? QStringLiteral("whole") : QStringLiteral("changed: ") + QString::fromLatin1(*got);
            } else {
                result = QStringLiteral("nothing reported (timeout)");
            }
            out << QStringLiteral("%1  %2  %3\n").arg(sizeText(sequence(n, st).size()), -10)
                       .arg(st ? QStringLiteral("ST") : QStringLiteral("BEL"), -4).arg(result);
            out.flush();
            b.writeError.clear();
        }
    }

    b.write("Q\r");
    b.close();
    out << "\n";
    return 0;
}
