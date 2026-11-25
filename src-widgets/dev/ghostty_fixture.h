/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

// Ghostty's annotated hex fixtures (ghostty-snapshot/*.hex), for the tests.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

struct GhosttyFixture
{
    QString name;
    QString type;        // the Kaitai type the bytes hold
    QStringList params;  // and its parameters
    int offset = 0;      // where that type starts
    QByteArray data;
    bool ok = false;     // the file could be read
};

inline GhosttyFixture loadGhosttyFixture(const QString &dir, const QString &name)
{
    GhosttyFixture f;
    f.name = name;
    QFile file(QDir(dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        return f;
    QByteArray hex;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const qsizetype hash = line.indexOf('#');
        const QByteArray content = hash < 0 ? line : line.left(hash);
        const QString comment = hash < 0 ? QString() : QString::fromUtf8(line.mid(hash + 1)).trimmed();
        hex += content.simplified().replace(' ', "");
        if (comment.startsWith(QLatin1String("Kaitai type:")))
            f.type = comment.mid(12).trimmed();
        else if (comment.startsWith(QLatin1String("Kaitai params:")))
            f.params = comment.mid(14).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        else if (comment.startsWith(QLatin1String("Kaitai offset:")))
            f.offset = comment.mid(14).trimmed().toInt(nullptr, 0);
    }
    f.data = QByteArray::fromHex(hex);
    f.ok = true;
    return f;
}
