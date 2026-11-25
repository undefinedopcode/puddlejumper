/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * ghostty-snap: a byte stream through a real Ghostty terminal, out as a
 * GHOSTSNP snapshot, as a Rex server would send it. For trying the decoder
 * and replay on whatever a program prints:
 *
 *   script -q -c 'ls --color=always' /dev/null | ghostty-snap 80 24 > ls.snap
 *   test-ghosttysnapshot --dump ls.snap
 */

#include "ghosttyvt.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    bool okColumns = false, okRows = false;
    const int columns = args.value(1).toInt(&okColumns);
    const int rows = args.value(2).toInt(&okRows);
    if (args.size() < 3 || args.size() > 4 || !okColumns || !okRows || columns < 1 || rows < 1
        || columns > 65535 || rows > 65535) {
        QTextStream(stderr) << "usage: ghostty-snap COLUMNS ROWS [INPUT] > SNAPSHOT\n";
        return 2;
    }
    QFile in;
    if (args.size() == 4) {
        in.setFileName(args[3]);
        if (!in.open(QIODevice::ReadOnly)) {
            QTextStream(stderr) << "ghostty-snap: can't read " << args[3] << "\n";
            return 1;
        }
    } else if (!in.open(stdin, QIODevice::ReadOnly)) {
        return 1;
    }

    GhosttyVt vt{quint16(columns), quint16(rows)};
    if (!vt.ok()) {
        QTextStream(stderr) << "ghostty-snap: no terminal\n";
        return 1;
    }
    vt.write(in.readAll());
    const QByteArray snap = vt.snapshot();
    if (snap.isEmpty()) {
        QTextStream(stderr) << "ghostty-snap: encoding failed\n";
        return 1;
    }
    QFile out;
    if (!out.open(stdout, QIODevice::WriteOnly) || out.write(snap) != snap.size())
        return 1;
    return 0;
}
