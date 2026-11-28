/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * probe-kitty: kitty's graphics protocol through a Rex-compatible server.
 * Inside a Rex block it is the server's terminal that a program talks to,
 * so for puddle to draw real pictures there:
 *
 *   query     the server's terminal answers kitty's support query (a=q);
 *   live      a picture the program sends reaches this client's stream;
 *   snapshot  a client that attaches afterwards gets the picture, with its
 *             pixels intact and placed, in the snapshot it starts from, on
 *             the primary screen and on the alternate one (where puddle
 *             runs), including a large one sent in chunks;
 *   delete    a deleted picture is gone from the next snapshot.
 *
 * A block runs dev/kitty-helper.py. It creates one session and destroys it.
 * A report, not a pass/fail test.
 *   ./build/probe-kitty unix:///run/user/1000/rex/server.sock
 */

#include "rexprobe.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QFileInfo>

#include <tuple>

using namespace rexprobe;

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

// The helper's pixels (see kitty-helper.py's picture()).
QByteArray picture(int w, int h)
{
    QByteArray px(qsizetype(w) * h * 4, Qt::Uninitialized);
    for (qsizetype i = 0; i < qsizetype(w) * h; ++i) {
        px[i * 4] = char((i * 7) & 255);
        px[i * 4 + 1] = char((i * 13 + 64) & 255);
        px[i * 4 + 2] = char((i * 29 + 128) & 255);
        px[i * 4 + 3] = char(255);
    }
    return px;
}

QByteArray hash16(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex().left(16); }

void row(const QString &what, const QString &result)
{
    out << QStringLiteral("  %1  %2\n").arg(what, -44).arg(result);
    out.flush();
}

// What the latest snapshot holds of picture `id` on screen `key`.
QString inSnapshot(const Block &b, quint16 key, quint32 id, int w, int h)
{
    const GhosttySnapshot::Screen *s = b.snapshot.screen(key);
    if (!s)
        return QStringLiteral("no such screen in the snapshot");
    if (!s->kitty)
        return QStringLiteral("screen has no kitty storage");
    const GhosttySnapshot::KittyImage *img = nullptr;
    for (const auto &i : s->kitty->images)
        if (i.id == id)
            img = &i;
    int placed = 0;
    QString where;
    for (const auto &p : s->kitty->placements) {
        if (p.key.imageId == id) {
            ++placed;
            where = QStringLiteral("%1x%2 cells").arg(p.columns).arg(p.rows);
        }
    }
    if (!img)
        return placed ? QStringLiteral("no picture, but %1 placement(s)").arg(placed) : QStringLiteral("not there");
    QString r = QStringLiteral("%1x%2").arg(img->width).arg(img->height);
    if (int(img->width) != w || int(img->height) != h)
        r += QStringLiteral(" (sent %1x%2)").arg(w).arg(h);
    r += img->format == GhosttySnapshot::KittyRgba ? QStringLiteral(", rgba") : QStringLiteral(", format %1").arg(int(img->format));
    if (img->data.isEmpty())
        r += QStringLiteral(", NO PIXELS");
    else
        r += hash16(img->data) == hash16(picture(w, h)) ? QStringLiteral(", pixels intact") : QStringLiteral(", PIXELS DIFFER");
    r += placed ? QStringLiteral(", placed %1").arg(where) : QStringLiteral(", NOT PLACED");
    if (placed > 1)
        r += QStringLiteral(" (%1 placements)").arg(placed);
    return r;
}

// Whether replaying the snapshot (what a client feeds its emulator) sends it.
QString inReplay(const Block &b, quint32 id)
{
    const QByteArray replay = GhosttySnapshot::replay(b.snapshot);
    const QByteArray key = "i=" + QByteArray::number(id);
    qsizetype at = 0;
    bool sent = false, placed = false;
    while ((at = replay.indexOf("\x1b_G", at)) >= 0) {
        const qsizetype semi = replay.indexOf(';', at);
        const qsizetype end = replay.indexOf("\x1b\\", at);
        const QByteArray keys = replay.mid(at + 3, (semi >= 0 && semi < end ? semi : end) - at - 3);
        if (("," + keys + ",").contains("," + key + ",")) {
            sent |= keys.contains("a=t") || keys.contains("a=T");
            placed |= keys.contains("a=p") || keys.contains("a=T");
        }
        at = end < 0 ? replay.size() : end;
    }
    return QStringLiteral("replay %1, %2").arg(sent ? QStringLiteral("sends it") : QStringLiteral("doesn't send it"),
                                                 placed ? QStringLiteral("places it") : QStringLiteral("doesn't place it"));
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QString server = argc > 1 ? QString::fromLocal8Bit(argv[1]) : qEnvironmentVariable("PJ_REX_SERVER");
    if (server.isEmpty()) {
        out << "usage: probe-kitty unix:///path | http://host:port (or PJ_REX_SERVER)\n";
        return 2;
    }
    const QString helper = QStringLiteral(PJ_KITTY_HELPER);
    if (!QFileInfo::exists(helper)) {
        err << "no helper at " << helper << "\n";
        return 2;
    }

    Block b;
    if (!b.open(server, {QStringLiteral("python3"), QStringLiteral("-u"), helper}, QStringLiteral("probe-kitty"))) {
        err << b.error() << "\n";
        return 2;
    }
    out << "server: " << server << "\n";
    waitFor([&] { return b.stream.contains("READY"); }, 3000);

    // Sends a command and waits for the helper's line starting with `prefix`.
    auto command = [&](const QByteArray &cmd, const QByteArray &prefix, int ms = 15000) -> QString {
        const qsizetype mark = b.stream.size();
        if (!b.write(cmd + "\r"))
            return QStringLiteral("write failed: ") + b.writeError;
        const auto got = b.lineAfter(mark, prefix, ms);
        return got ? QString::fromLatin1(got->mid(prefix.size())) : QStringLiteral("no answer (timeout)");
    };
    auto snapshotRow = [&](const QString &what, quint16 key, quint32 id, int w, int h) {
        if (!b.reopen()) {
            row(what, QStringLiteral("no fresh snapshot: ") + b.failure);
            return;
        }
        row(what, inSnapshot(b, key, id, w, h) + QStringLiteral("; ") + inReplay(b, id));
    };

    out << "\nquery\n";
    row(QStringLiteral("kitty support query (a=q), reply"), command("K", "KQ "));

    out << "\nprimary screen\n";
    {
        const qsizetype mark = b.stream.size();
        row(QStringLiteral("send and show 4x2 picture, id 7: reply"), command("T 7", "TX 7 "));
        row(QStringLiteral("the command in this client's live stream"),
            b.stream.indexOf("\x1b_Ga=T,f=32,s=4,v=2,c=8,r=2,i=7;", mark) >= 0 ? QStringLiteral("yes, as sent") : QStringLiteral("no"));
    }
    snapshotRow(QStringLiteral("fresh snapshot: picture 7 (screen 0)"), 0, 7, 4, 2);

    out << "\nalternate screen (where puddle runs)\n";
    row(QStringLiteral("enter the alternate screen"), command("A", "ALT") == QString() ? QStringLiteral("yes") : QStringLiteral("?"));
    row(QStringLiteral("send and show 4x2 picture, id 8: reply"), command("T 8", "TX 8 "));
    snapshotRow(QStringLiteral("fresh snapshot: picture 8 (screen 1)"), 1, 8, 4, 2);
    row(QStringLiteral("snapshot's active screen"), b.snapshot.terminal.activeScreen == 1 ? QStringLiteral("alternate") : QStringLiteral("primary"));
    row(QStringLiteral("snapshot: picture 7 still on screen 0"), inSnapshot(b, 0, 7, 4, 2));

    out << "\nlarge picture, chunked (as puddle would send one)\n";
    for (const auto &[id, w, h] : {std::tuple{9, 256, 256}, std::tuple{10, 1024, 1024}}) {
        const QByteArray want = hash16(picture(w, h));
        QElapsedTimer t;
        t.start();
        const qsizetype mark = b.stream.size();
        const QString big = command("B " + QByteArray::number(id) + " " + QByteArray::number(w) + " " + QByteArray::number(h),
                                    "BIG " + QByteArray::number(id) + " ", 15000);
        // The helper is sending now: wait for its line without typing anything.
        const QByteArray tx = "TX " + QByteArray::number(id) + " ";
        const auto got = b.lineAfter(mark, tx, 60000);
        const QString reply = got ? QString::fromLatin1(got->mid(tx.size())) : QStringLiteral("no answer (timeout)");
        row(QStringLiteral("send %1x%2 rgba (%3 MiB), id %4: reply")
                .arg(w).arg(h).arg(double(w) * h * 4 / (1024 * 1024), 0, 'f', 2).arg(id),
            QStringLiteral("%1 (%2 ms)%3").arg(reply).arg(t.elapsed())
                .arg(big == QString::fromLatin1(want) ? QString() : QStringLiteral("; helper hash mismatch")));
        snapshotRow(QStringLiteral("fresh snapshot: picture %1 (screen 1)").arg(id), 1, id, w, h);
    }

    out << "\ndelete\n";
    row(QStringLiteral("delete picture 8 (a=d,d=I): reply"), command("D 8", "DEL 8 "));
    snapshotRow(QStringLiteral("fresh snapshot: picture 8 (screen 1)"), 1, 8, 4, 2);

    out << "\nback to the primary screen\n";
    command("P", "PRIMARY");
    snapshotRow(QStringLiteral("fresh snapshot: picture 7 (screen 0)"), 0, 7, 4, 2);
    {
        const GhosttySnapshot::Screen *alt = b.snapshot.screen(1);
        int images = 0;
        if (alt && alt->kitty)
            images = int(alt->kitty->images.size());
        row(QStringLiteral("alternate screen's pictures after leaving it"),
            alt ? QStringLiteral("%1 picture(s)").arg(images) : QStringLiteral("no alternate screen in the snapshot"));
    }

    if (!b.failure.isEmpty())
        out << "\nstream failed: " << b.failure << "\n";
    b.write("Q\r");
    b.close();
    out << "\n";
    return 0;
}
