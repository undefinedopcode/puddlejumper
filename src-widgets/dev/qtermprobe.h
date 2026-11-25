/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

// qtermwidget's emulator with no window, for the tests: bytes in, screens
// and their lines out.

#include "Character.h"
#include "History.h"
#include "Screen.h"
#include "Vt102Emulation.h"

#include <QEventLoop>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QTimer>

class QtermProbe : public Konsole::Vt102Emulation
{
public:
    QtermProbe(int columns, int rows)
    {
        setHistory(Konsole::HistoryTypeBuffer(10000));
        setImageSize(rows, columns);
        setCellPixelSize(QSize(10, 20));
        connect(this, &Emulation::titleChanged, this, [this](int what, const QString &text) {
            titles[what] = text;
        });
    }
    void feed(const QByteArray &bytes) { receiveData(bytes.constData(), int(bytes.size())); }
    Konsole::Screen *screen(int i) const { return _screen[i]; }
    Konsole::Screen *showing() const { return _currentScreen; }
    // Title changes are sent on a timer.
    void settle()
    {
        QEventLoop loop;
        QTimer::singleShot(60, &loop, &QEventLoop::quit);
        loop.exec();
    }
    QHash<int, QString> titles;
};

namespace QtermProbeLines {

// A screen's lines, history first, without trailing blanks.
inline QStringList screenLines(const Konsole::Screen *s, QList<bool> *wrapped = nullptr)
{
    const int total = s->getHistLines() + s->getLines();
    const int columns = s->getColumns();
    QList<Konsole::Character> image(total * columns);
    s->getImage(image.data(), int(image.size()), 0, total - 1);
    const QVector<Konsole::LineProperty> props = s->getLineProperties(0, total - 1);
    QStringList out;
    for (int y = 0; y < total; ++y) {
        QString line;
        for (int x = 0; x < columns; ++x) {
            const Konsole::Character &c = image[y * columns + x];
            if (c.rendition & RE_EXTENDED_CHAR) {
                ushort length = 0;
                const uint *chars = Konsole::ExtendedCharTable::instance.lookupExtendedChar(c.character, length);
                for (ushort i = 0; chars && i < length; ++i)
                    line += QString::fromUcs4(reinterpret_cast<const char32_t *>(&chars[i]), 1);
            } else if (c.character != 0) {
                line += QString::fromUcs4(&c.character, 1);
            }
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        out << line;
        if (wrapped)
            wrapped->append(y < props.size() && (props[y] & Konsole::LINE_WRAPPED));
    }
    return out;
}

// Every cell of a screen, history first, a row of `columns` at a time.
inline QList<Konsole::Character> screenCells(const Konsole::Screen *s)
{
    const int total = s->getHistLines() + s->getLines();
    QList<Konsole::Character> image(total * s->getColumns());
    s->getImage(image.data(), int(image.size()), 0, total - 1);
    return image;
}

} // namespace QtermProbeLines
