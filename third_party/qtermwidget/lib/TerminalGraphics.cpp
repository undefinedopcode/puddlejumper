/*
 * puddlejumper: images in the terminal, by kitty's graphics protocol.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "TerminalGraphics.h"

#include <QtEndian>

#include <algorithm>
#include <climits>
#include <cmath>

using namespace Konsole;

namespace
{

// Larger than any screen; kitty's limit too.
constexpr int kMaxSide = 10000;
// A cell's size when the display hasn't said (it always should).
const QSize kFallbackCell(10, 20);

bool parseInt(const QByteArray &value, qint64 min, qint64 max, qint64 &out)
{
    bool ok = false;
    out = value.toLongLong(&ok);
    return ok && out >= min && out <= max;
}

} // namespace

bool TerminalGraphics::parse(const QByteArray &control, Command &cmd)
{
    if (control.isEmpty())
        return true;
    for (const QByteArray &pair : control.split(',')) {
        if (pair.size() < 3 || pair.at(1) != '=')
            return false;
        const char key = pair.at(0);
        const QByteArray value = pair.mid(2);
        // Keys with a letter for a value.
        if (key == 'a' || key == 't' || key == 'o' || key == 'd' || key == 'e') {
            if (value.size() != 1)
                return false;
            switch (key) {
            case 'e': cmd.featureAction = value.at(0); break;
            case 'a': cmd.action = value.at(0); cmd.hasAction = true; break;
            case 't': cmd.medium = value.at(0); break;
            case 'o': cmd.compression = value.at(0); break;
            case 'd': cmd.deleteWhat = value.at(0); break;
            }
            continue;
        }
        qint64 n = 0;
        const qint64 uintMax = 0xffffffffLL;
        if (!parseInt(value, key == 'z' || key == 'H' || key == 'V' ? -0x80000000LL : 0,
                      key == 'z' || key == 'H' || key == 'V' ? 0x7fffffffLL : uintMax, n))
            return false;
        switch (key) {
        case 'f': cmd.format = int(n); break;
        case 'q': cmd.quiet = int(n); break;
        case 'm': cmd.more = n == 1; break;
        case 'i': cmd.id = quint32(n); cmd.hasId = n != 0; break;
        case 'I': cmd.number = quint32(n); cmd.hasNumber = n != 0; break;
        case 'p': cmd.placementId = quint32(n); cmd.hasPlacementId = n != 0; break;
        case 's': cmd.width = int(qMin<qint64>(n, INT_MAX)); break;
        case 'v': cmd.height = int(qMin<qint64>(n, INT_MAX)); break;
        case 'S': cmd.dataSize = int(qMin<qint64>(n, INT_MAX)); break;
        case 'x': cmd.x = int(qMin<qint64>(n, INT_MAX)); break;
        case 'y': cmd.y = int(qMin<qint64>(n, INT_MAX)); break;
        case 'w': cmd.w = int(qMin<qint64>(n, INT_MAX)); break;
        case 'h': cmd.h = int(qMin<qint64>(n, INT_MAX)); break;
        case 'X': cmd.cellX = int(qMin<qint64>(n, INT_MAX)); break;
        case 'Y': cmd.cellY = int(qMin<qint64>(n, INT_MAX)); break;
        case 'c': cmd.columns = int(qMin<qint64>(n, 1000)); break;
        case 'r': cmd.rows = int(qMin<qint64>(n, 1000)); break;
        case 'z': cmd.z = qint32(n); cmd.hasZ = true; break;
        case 'C': cmd.noCursorMove = n == 1; break;
        case 'U': cmd.unicodePlaceholder = n == 1; break;
        case 'P': case 'Q': case 'H': case 'V': cmd.relative = cmd.relative || n != 0; break;
        case 'e': break;    // a letter; see below
        default: break;   // ones this doesn't know, or that don't matter here (O, N, e)
        }
    }
    return true;
}

QByteArray TerminalGraphics::featureReply(const Command &cmd, const QByteArray &control)
{
    if (!cmd.hasId)
        return {};   // a query needs one
    const char action = cmd.featureAction;
    const bool placing = action == 'T' || action == 't' || action == 'p';
    QByteArray keys;
    if (placing || action == 'd') {
        keys = ",e=" + QByteArray(1, action);
        for (const QByteArray &pair : control.split(',')) {
            if (pair.size() < 3 || pair.at(1) != '=')
                continue;
            const char key = pair.at(0);
            const QByteArray value = pair.mid(2);
            bool known = false;
            if (placing) {
                switch (key) {
                case 'f': known = value == "24" || value == "32" || value == "100"; break;
                case 't': known = value == "d"; break;
                case 'o': known = value == "z"; break;
                case 'U': case 'C': known = value == "1"; break;
                case 's': case 'v': case 'S': case 'x': case 'y': case 'w': case 'h':
                case 'X': case 'Y': case 'c': case 'r': case 'z': case 'p':
                    known = true;
                    break;
                default: break;   // P, Q, H, V (relative placements), N and the rest
                }
            } else {
                known = key == 'd' && value.size() == 1 && QByteArray("aAiInNcCpPqQxXyYzZrR").contains(value.at(0));
                known = known || (key != 'd' && QByteArray("ixyzpI").contains(key));
            }
            // Defaults, and a, i, I, m, q and e themselves, aren't listed.
            if (known && value != "0" && !QByteArray("aiImqe").contains(key))
                keys += ',' + pair;
        }
    }
    return "\033_Gi=" + QByteArray::number(cmd.id) + keys + ";OK\033\\";
}

QByteArray TerminalGraphics::reply(const Command &cmd, quint32 id, const QByteArray &message)
{
    // Only to a command that named its image, and as quiet as it asked.
    if (!cmd.hasId && !cmd.hasNumber)
        return {};
    const bool ok = message == "OK";
    if ((ok && cmd.quiet >= 1) || cmd.quiet >= 2)
        return {};
    QByteArray out = "\033_Gi=" + QByteArray::number(id);
    if (cmd.hasNumber)
        out += ",I=" + QByteArray::number(cmd.number);
    if (cmd.hasPlacementId)
        out += ",p=" + QByteArray::number(cmd.placementId);
    return out + ';' + message + "\033\\";
}

TerminalGraphics::Result TerminalGraphics::command(const QByteArray &body, const Context &context)
{
    const int semi = body.indexOf(';');
    const QByteArray control = semi < 0 ? body : body.left(semi);
    const QByteArray payload = semi < 0 ? QByteArray() : body.mid(semi + 1);

    Command cmd;
    if (!parse(control, cmd)) {
        Result r;
        r.reply = reply(cmd, cmd.id, "EINVAL:Malformed control data");
        return r;
    }

    const auto base64 = QByteArray::fromBase64Encoding(payload, QByteArray::AbortOnBase64DecodingErrors);
    if (!base64) {
        Result r;
        r.reply = m_loading ? reply(m_load, m_load.id, "EINVAL:Payload is not base64")
                            : reply(cmd, cmd.id, "EINVAL:Payload is not base64");
        m_loading = false;
        m_loadData.clear();
        return r;
    }

    // A feature query (e=): which keys, with these values, this understands
    // for that action. No data, no effect, not even on an image coming in.
    if (cmd.action == 'q' && cmd.featureAction) {
        Result r;
        r.reply = featureReply(cmd, control);
        return r;
    }

    // The rest of an image coming in chunks: only m (and q) after the first.
    if (m_loading) {
        if (!cmd.hasAction && !cmd.hasId && !cmd.hasNumber) {
            if (m_loadData.size() + base64.decoded.size() > kQuota) {
                m_loading = false;
                m_loadData.clear();
                Result r;
                r.reply = reply(m_load, m_load.id, "ENOSPC:Image too large");
                return r;
            }
            m_loadData += *base64;
            if (cmd.quiet)
                m_load.quiet = cmd.quiet;
            if (cmd.more)
                return {};
            m_loading = false;
            const QByteArray data = std::move(m_loadData);
            m_loadData.clear();
            return transmit(m_load, data, context);
        }
        // Anything else abandons it (a delete, as the spec says; anything,
        // rather than take a new command's data for the old image's).
        m_loading = false;
        m_loadData.clear();
    }

    switch (cmd.action) {
    case 't':
    case 'T':
    case 'q':
        if (cmd.medium != 'd') {
            Result r;
            r.reply = reply(cmd, cmd.id, "EINVAL:Only direct transmission is supported");
            return r;
        }
        if (cmd.more) {
            m_loading = true;
            m_load = cmd;
            m_loadData = *base64;
            return {};
        }
        return transmit(cmd, *base64, context);
    case 'p': {
        const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
        return put(cmd, id, context);
    }
    case 'd':
        return remove(cmd, context);
    case 'f':
    case 'a':
    case 'c': {
        Result r;
        r.reply = reply(cmd, cmd.id, "EINVAL:Animation is not supported");
        return r;
    }
    default: {
        Result r;
        r.reply = reply(cmd, cmd.id, "EINVAL:Unknown action");
        return r;
    }
    }
}

QByteArray TerminalGraphics::decode(const Command &cmd, const QByteArray &data, QImage &image) const
{
    int bytesPerPixel = 0;
    if (cmd.format == 32)
        bytesPerPixel = 4;
    else if (cmd.format == 24)
        bytesPerPixel = 3;
    else if (cmd.format != 100)
        return "EINVAL:Unsupported format";
    if (bytesPerPixel && (cmd.width <= 0 || cmd.height <= 0 || cmd.width > kMaxSide || cmd.height > kMaxSide))
        return "EINVAL:Bad image size";

    QByteArray raw = data;
    if (cmd.compression == 'z') {
        // qUncompress() wants the size up front; a guess grows as need be.
        const qint64 expected = bytesPerPixel ? qint64(cmd.width) * cmd.height * bytesPerPixel
                                              : (cmd.dataSize > 0 ? cmd.dataSize : data.size() * 4LL);
        if (expected > kQuota)
            return "ENOSPC:Image too large";
        QByteArray framed(4, Qt::Uninitialized);
        qToBigEndian<quint32>(quint32(expected), framed.data());
        raw = qUncompress(framed + data);
        if (raw.isEmpty())
            return "EINVAL:Failed to decompress";
    } else if (cmd.compression) {
        return "EINVAL:Unsupported compression";
    }

    if (bytesPerPixel) {
        const qint64 needed = qint64(cmd.width) * cmd.height * bytesPerPixel;
        if (raw.size() < needed)
            return "ENODATA:Insufficient image data";
        image = QImage(reinterpret_cast<const uchar *>(raw.constData()), cmd.width, cmd.height,
                       cmd.width * bytesPerPixel,
                       bytesPerPixel == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888)
                    .copy();   // off the buffer, which goes
    } else if (!image.loadFromData(raw, "PNG")) {
        return "EBADPNG:Failed to load PNG data";
    }
    if (image.isNull() || image.width() > kMaxSide || image.height() > kMaxSide)
        return "EINVAL:Bad image size";
    // Drawn many times; converted once.
    image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                          : QImage::Format_RGB32);
    return {};
}

TerminalGraphics::Result TerminalGraphics::transmit(const Command &cmd, const QByteArray &data,
                                                    const Context &context)
{
    Result r;
    QImage image;
    const QByteArray error = decode(cmd, data, image);
    if (!error.isEmpty()) {
        r.reply = reply(cmd, cmd.id, error);
        return r;
    }
    // A query: the data checked, nothing kept.
    if (cmd.action == 'q') {
        r.reply = reply(cmd, cmd.id, "OK");
        return r;
    }

    const quint32 id = cmd.hasId ? cmd.id : freeId();
    dropImage(id);   // the same id again replaces it
    makeRoom(image.sizeInBytes());
    m_images.insert(id, Image{image, cmd.number, ++m_serial});

    if (cmd.action == 'T')
        return put(cmd, id, context);
    r.reply = reply(cmd, id, "OK");
    return r;
}

TerminalGraphics::Result TerminalGraphics::put(const Command &cmd, quint32 id, const Context &context)
{
    Result r;
    if (cmd.relative) {
        r.reply = reply(cmd, id, "EINVAL:Relative placements are not supported");
        return r;
    }
    const auto it = m_images.constFind(id);
    if (id == 0 || it == m_images.constEnd()) {
        r.reply = reply(cmd, id, "ENOENT:No such image");
        return r;
    }

    const QRect full = it->image.rect();
    const QRect source = QRect(cmd.x, cmd.y,
                               cmd.w > 0 ? cmd.w : full.width() - cmd.x,
                               cmd.h > 0 ? cmd.h : full.height() - cmd.y) & full;
    if (source.isEmpty()) {
        r.reply = reply(cmd, id, "EINVAL:Source rectangle is empty");
        return r;
    }

    const QSize cell = context.cellSize.isValid() && !context.cellSize.isEmpty()
                           ? context.cellSize : kFallbackCell;
    GraphicsPlacement p;
    p.imageId = id;
    p.placementId = cmd.placementId;
    p.row = context.cursorRow;
    p.column = context.cursorColumn;
    p.source = source;
    p.offsetX = qreal(qMin(cmd.cellX, cell.width() - 1)) / cell.width();
    p.offsetY = qreal(qMin(cmd.cellY, cell.height() - 1)) / cell.height();
    // Given columns and rows, it fills them; given one, the other keeps its
    // shape; given neither, it shows at its own size.
    // Given columns and rows, it's fitted in them, its shape kept (letter-
    // or pillarboxed, centred); given one, the other follows from its shape;
    // given neither, it shows at its own size. The offset adds no cells.
    const qreal naturalW = qreal(source.width()) / cell.width();
    const qreal naturalH = qreal(source.height()) / cell.height();
    if (cmd.columns > 0 && cmd.rows > 0) {
        const qreal scale = qMin(cmd.columns / naturalW, cmd.rows / naturalH);
        p.width = naturalW * scale;
        p.height = naturalH * scale;
        p.offsetX += (cmd.columns - p.width) / 2;
        p.offsetY += (cmd.rows - p.height) / 2;
    } else if (cmd.columns > 0) {
        p.width = cmd.columns;
        p.height = naturalH * cmd.columns / naturalW;
    } else if (cmd.rows > 0) {
        p.height = cmd.rows;
        p.width = naturalW * cmd.rows / naturalH;
    } else {
        p.width = naturalW;
        p.height = naturalH;
    }
    p.columns = cmd.columns > 0 ? cmd.columns : qMax(1, int(std::ceil(p.offsetX + p.width - 1e-6)));
    p.rows = cmd.rows > 0 ? cmd.rows : qMax(1, int(std::ceil(p.offsetY + p.height - 1e-6)));
    p.z = cmd.z;
    p.serial = ++m_serial;
    p.isVirtual = cmd.unicodePlaceholder;

    // The same placement id again moves it.
    if (cmd.hasPlacementId) {
        removePlacements([&](const GraphicsPlacement &o) {
            return o.imageId == id && o.placementId == cmd.placementId;
        }, false);
    }
    m_placements.append(p);
    ++m_generation;

    r.changed = true;
    r.reply = reply(cmd, id, "OK");
    if (!cmd.noCursorMove && !p.isVirtual) {
        r.moveCursor = true;
        r.cursorColumns = p.columns;
        r.cursorRows = p.rows;
    }
    return r;
}

template <typename Match>
void TerminalGraphics::removePlacements(Match match, bool freeImages)
{
    QList<quint32> touched;
    for (qsizetype i = m_placements.size() - 1; i >= 0; --i) {
        if (match(m_placements.at(i))) {
            touched << m_placements.at(i).imageId;
            m_placements.removeAt(i);
        }
    }
    if (touched.isEmpty())
        return;
    ++m_generation;
    if (!freeImages)
        return;
    for (quint32 id : std::as_const(touched)) {
        const bool used = std::any_of(m_placements.cbegin(), m_placements.cend(),
                                      [id](const GraphicsPlacement &p) { return p.imageId == id; });
        if (!used)
            m_images.remove(id);
    }
}

TerminalGraphics::Result TerminalGraphics::remove(const Command &cmd, const Context &context)
{
    // Lower case keeps the images for putting again; upper case also frees
    // those left with no placement.
    const bool freeImages = cmd.deleteWhat >= 'A' && cmd.deleteWhat <= 'Z';
    const char what = char(cmd.deleteWhat | 0x20);
    const quint64 before = m_generation;

    auto covers = [](const GraphicsPlacement &p, int column, int row) {
        return !p.isVirtual && column >= p.column && column < p.column + p.columns
            && row >= p.row && row < p.row + p.rows;
    };
    auto byImage = [&](quint32 id) {
        removePlacements([&](const GraphicsPlacement &p) {
            return p.imageId == id && (!cmd.hasPlacementId || p.placementId == cmd.placementId);
        }, false);
        // Upper case frees the image even when it had no placement.
        if (freeImages && !std::any_of(m_placements.cbegin(), m_placements.cend(),
                                       [id](const GraphicsPlacement &p) { return p.imageId == id; })
            && m_images.remove(id))
            ++m_generation;
    };

    switch (what) {
    case 'a':   // all on screen
        removePlacements([&](const GraphicsPlacement &p) {
            return !p.isVirtual && p.row < context.lines && p.row + p.rows > 0;
        }, freeImages);
        break;
    case 'i':
        byImage(cmd.id);
        break;
    case 'n':
        byImage(newestWithNumber(cmd.number));
        break;
    case 'c':
        removePlacements([&](const GraphicsPlacement &p) {
            return covers(p, context.cursorColumn, context.cursorRow);
        }, freeImages);
        break;
    case 'p':
    case 'q':
        removePlacements([&](const GraphicsPlacement &p) {
            return covers(p, cmd.x - 1, cmd.y - 1) && (what == 'p' || p.z == cmd.z);
        }, freeImages);
        break;
    case 'x':
        removePlacements([&](const GraphicsPlacement &p) {
            return !p.isVirtual && cmd.x - 1 >= p.column && cmd.x - 1 < p.column + p.columns;
        }, freeImages);
        break;
    case 'y':
        removePlacements([&](const GraphicsPlacement &p) {
            return !p.isVirtual && cmd.y - 1 >= p.row && cmd.y - 1 < p.row + p.rows;
        }, freeImages);
        break;
    case 'z':
        removePlacements([&](const GraphicsPlacement &p) {
            return !p.isVirtual && p.z == cmd.z;
        }, freeImages);
        break;
    case 'r':
        removePlacements([&](const GraphicsPlacement &p) {
            return p.imageId >= quint32(cmd.x) && p.imageId <= quint32(cmd.y);
        }, freeImages);
        break;
    default:    // f: animation frames, which there are none of
        break;
    }

    Result r;
    r.changed = m_generation != before;
    return r;
}

void TerminalGraphics::dropImage(quint32 id)
{
    if (!m_images.remove(id))
        return;
    removePlacements([id](const GraphicsPlacement &p) { return p.imageId == id; }, false);
    ++m_generation;
}

void TerminalGraphics::makeRoom(qint64 bytes)
{
    qint64 used = 0;
    for (const Image &image : std::as_const(m_images))
        used += image.image.sizeInBytes();
    while (used + bytes > kQuota && !m_images.isEmpty()) {
        // The oldest no longer shown, else the oldest.
        quint32 victim = 0;
        quint64 oldest = ~0ULL;
        bool victimPlaced = true;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            const quint32 id = it.key();
            const bool placed = std::any_of(m_placements.cbegin(), m_placements.cend(),
                                            [id](const GraphicsPlacement &p) { return p.imageId == id; });
            if ((victimPlaced && !placed) || (placed == victimPlaced && it->serial < oldest)) {
                victim = id;
                oldest = it->serial;
                victimPlaced = placed;
            }
        }
        used -= m_images.value(victim).image.sizeInBytes();
        dropImage(victim);
    }
}

quint32 TerminalGraphics::newestWithNumber(quint32 number) const
{
    quint32 id = 0;
    quint64 newest = 0;
    for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
        if (it->number == number && it->serial >= newest) {
            id = it.key();
            newest = it->serial;
        }
    }
    return id;
}

quint32 TerminalGraphics::freeId() const
{
    // High, out of the way of the small ids programs pick.
    quint32 id = 0x80000000u + quint32(m_serial % 0x7fffffffu);
    while (id == 0 || m_images.contains(id))
        ++id;
    return id;
}

void TerminalGraphics::linesMoved(int first, int last, int delta)
{
    if (delta == 0 || m_placements.isEmpty())
        return;
    // The lines written over that weren't themselves moved.
    const int lo = delta < 0 ? first + delta : last + 1;
    const int hi = delta < 0 ? first - 1 : last + delta;
    bool changed = false;
    for (qsizetype i = m_placements.size() - 1; i >= 0; --i) {
        GraphicsPlacement &p = m_placements[i];
        if (p.isVirtual)
            continue;
        if (p.row >= first && p.row <= last) {
            p.row += delta;
            changed = true;
        } else if (p.row >= lo && p.row <= hi) {
            m_placements.removeAt(i);
            changed = true;
        }
    }
    if (changed)
        ++m_generation;
}

void TerminalGraphics::historyLineAdded(int historyLines)
{
    bool changed = false;
    for (GraphicsPlacement &p : m_placements) {
        if (!p.isVirtual && p.row <= 0) {
            --p.row;
            changed = true;
        }
    }
    if (changed)
        ++m_generation;
    prune(historyLines);
}

void TerminalGraphics::prune(int historyLines)
{
    // Gone from the top of the history, all of it.
    removePlacements([historyLines](const GraphicsPlacement &p) {
        return !p.isVirtual && p.row + p.rows <= -historyLines;
    }, false);
}

void TerminalGraphics::screenCleared(int lines)
{
    removePlacements([lines](const GraphicsPlacement &p) {
        return !p.isVirtual && p.row < lines && p.row + p.rows > 0;
    }, false);
}

void TerminalGraphics::historyCleared()
{
    removePlacements([](const GraphicsPlacement &p) { return !p.isVirtual && p.row < 0; }, false);
}

void TerminalGraphics::historyResized(int historyLines)
{
    prune(historyLines);
}

void TerminalGraphics::clear()
{
    if (!m_images.isEmpty() || !m_placements.isEmpty())
        ++m_generation;
    m_images.clear();
    m_placements.clear();
    m_loading = false;
    m_loadData.clear();
}
