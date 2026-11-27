/*
 * puddlejumper: images in the terminal, by kitty's graphics protocol.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "TerminalGraphics.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QtEndian>

#include <algorithm>
#include <climits>
#include <cmath>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// Every failure to read a file or shared memory gets the same answer, so a
// program (perhaps on another machine) learns nothing of files it can't read.
const QByteArray kReadError = "EBADF:Failed to read image file";

// Places a path may not lead, merely opening some of which does things.
bool isSensitive(const QString &path)
{
    if (path.startsWith(QLatin1String("/dev/shm/")))
        return false;
    for (const char *dir : {"/proc", "/sys", "/dev"}) {
        const QString d = QLatin1String(dir);
        if (path == d || path.startsWith(d + QLatin1Char('/')))
            return true;
    }
    return false;
}

// Bytes [offset, offset + size) of something `total` long (size 0: the
// rest), or as many as there are: kitty reads what it can and finds out
// when decoding whether it was enough (kitten icat's queries give S as the
// path's length, not the file's).
bool span(qint64 total, qint64 offset, qint64 size, qint64 &wanted)
{
    if (offset < 0 || offset > total || size < 0)
        return false;
    wanted = size > 0 ? qMin(size, total - offset) : total - offset;
    return wanted <= Konsole::TerminalGraphics::kQuota;
}

bool readFile(const QString &real, qint64 offset, qint64 size, QByteArray &out)
{
    QFile file(real);
    qint64 wanted = 0;
    if (!file.open(QIODevice::ReadOnly) || file.isSequential()
        || !span(file.size(), offset, size, wanted) || !file.seek(offset))
        return false;
    out = file.read(wanted);
    return out.size() == wanted;
}

// A temporary file kitty's way: in a temporary directory, and named so.
bool isTemporary(const QString &real)
{
    if (!real.contains(QLatin1String("tty-graphics-protocol")))
        return false;
    QStringList dirs = {QStringLiteral("/tmp"), QStringLiteral("/dev/shm"), QDir::tempPath()};
    const QString env = qEnvironmentVariable("TMPDIR");
    if (!env.isEmpty())
        dirs << env;
    for (const QString &dir : std::as_const(dirs)) {
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (!canonical.isEmpty() && real.startsWith(canonical + QLatin1Char('/')))
            return true;
    }
    return false;
}

#ifdef Q_OS_UNIX
bool readSharedMemory(const QByteArray &name, qint64 offset, qint64 size, QByteArray &out)
{
    if (name.isEmpty() || name.contains('\0') || name.size() > 255)
        return false;
    const int fd = shm_open(name.constData(), O_RDONLY, 0);
    if (fd < 0)
        return false;
    bool ok = false;
    struct stat st;
    qint64 wanted = 0;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && span(st.st_size, offset, size, wanted)) {
        void *map = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            out = QByteArray(static_cast<const char *>(map) + offset, wanted);
            munmap(map, size_t(st.st_size));
            ok = true;
        }
    }
    // Read once: the program leaves it to the terminal to remove.
    close(fd);
    shm_unlink(name.constData());
    return ok;
}
#endif

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
        case 'O': cmd.dataOffset = n; break;
        case 'x': cmd.x = int(qMin<qint64>(n, INT_MAX)); break;
        case 'y': cmd.y = int(qMin<qint64>(n, INT_MAX)); break;
        case 'w': cmd.w = int(qMin<qint64>(n, INT_MAX)); break;
        case 'h': cmd.h = int(qMin<qint64>(n, INT_MAX)); break;
        case 'X': cmd.cellX = int(qMin<qint64>(n, INT_MAX)); cmd.keyX = quint32(n); break;
        case 'Y': cmd.cellY = int(qMin<qint64>(n, INT_MAX)); cmd.keyY = quint32(n); break;
        case 'c': cmd.columns = int(qMin<qint64>(n, 1000)); cmd.keyC = quint32(n); break;
        case 'r': cmd.rows = int(qMin<qint64>(n, 1000)); cmd.keyR = quint32(n); break;
        case 'z': cmd.z = qint32(n); cmd.hasZ = true; break;
        case 'C': cmd.noCursorMove = n == 1; break;
        case 'U': cmd.unicodePlaceholder = n == 1; break;
        case 'P': case 'Q': case 'H': case 'V': cmd.relative = cmd.relative || n != 0; break;
        default: break;   // ones this doesn't know, or that don't matter here (N)
        }
    }
    return true;
}

QByteArray TerminalGraphics::featureReply(const Command &cmd, const QByteArray &control)
{
    if (!cmd.hasId)
        return {};   // a query needs one
    const char action = cmd.featureAction;
    const bool placing = action == 'T' || action == 't' || action == 'p' || action == 'q';
    QByteArray keys;
    if (placing || QByteArray("dfac").contains(action)) {
        keys = ",e=" + QByteArray(1, action);
        for (const QByteArray &pair : control.split(',')) {
            if (pair.size() < 3 || pair.at(1) != '=')
                continue;
            const char key = pair.at(0);
            const QByteArray value = pair.mid(2);
            bool known = false;
#ifdef Q_OS_UNIX
            const QByteArray media = "dfts";
#else
            const QByteArray media = "dft";
#endif
            const bool medium = value.size() == 1 && media.contains(value.at(0));
            if (placing) {
                switch (key) {
                case 'f': known = value == "8" || value == "16" || value == "24" || value == "32" || value == "100"; break;
                case 't': known = medium; break;
                case 'o': known = value == "z"; break;
                case 'U': case 'C': known = value == "1"; break;
                case 's': case 'v': case 'S': case 'O': case 'x': case 'y': case 'w': case 'h':
                case 'X': case 'Y': case 'c': case 'r': case 'z': case 'p':
                    known = true;
                    break;
                default: break;   // P, Q, H, V (relative placements), N and the rest
                }
            } else if (action == 'f') {
                switch (key) {
                case 'f': known = value == "24" || value == "32" || value == "100"; break;
                case 't': known = medium; break;
                case 'o': known = value == "z"; break;
                case 'X': known = value == "1"; break;
                case 's': case 'v': case 'S': case 'O': case 'x': case 'y':
                case 'c': case 'r': case 'z': case 'Y':
                    known = true;
                    break;
                default: break;
                }
            } else if (action == 'a') {
                known = (key == 's' && (value == "1" || value == "2" || value == "3"))
                     || QByteArray("rzcv").contains(key);
            } else if (action == 'c') {
                known = (key == 'C' && value == "1") || QByteArray("rcxywhXY").contains(key);
            } else {
                known = key == 'd' && value.size() == 1 && QByteArray("aAiInNcCfFpPqQxXyYzZrR").contains(value.at(0));
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
    if (cmd.hasId && cmd.hasNumber) {
        Result r;
        r.reply = reply(cmd, cmd.id, "EINVAL:Both an image id and number given");
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

    // The rest of an image coming in chunks. The spec has only m (and q)
    // after the first, and a=f for a frame's; kitty takes any direct
    // transmission while one is coming in as more of it, its other keys
    // ignored, and kitten icat repeats a=T (and q) on every chunk.
    if (m_loading) {
        const bool continues = m_load.action == 'f'
            ? !cmd.hasAction || cmd.action == 'f'
            : (!cmd.hasAction || cmd.action == 't' || cmd.action == 'T' || cmd.action == 'q') && cmd.medium == 'd';
        if (continues) {
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
            return m_load.action == 'f' ? frame(m_load, data) : transmit(m_load, data, context);
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
    case 'f':
        if (cmd.more) {
            m_loading = true;
            m_load = cmd;
            m_loadData = *base64;
            return {};
        }
        return cmd.action == 'f' ? frame(cmd, *base64) : transmit(cmd, *base64, context);
    case 'p': {
        const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
        return put(cmd, id, context);
    }
    case 'd':
        return remove(cmd, context);
    case 'a':
        return animation(cmd);
    case 'c':
        return compose(cmd);
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
    else if (cmd.format == 16)
        bytesPerPixel = 2;
    else if (cmd.format == 8)
        bytesPerPixel = 1;
    else if (cmd.format != 100)
        return "EINVAL:Unsupported format";
    if (bytesPerPixel && (cmd.width <= 0 || cmd.height <= 0 || cmd.width > kMaxSide || cmd.height > kMaxSide))
        return "EINVAL:Bad image size";

    QByteArray raw = data;
    if (cmd.compression == 'z') {
        // qUncompress() wants the size up front; a guess grows as need be.
        // (S is the PNG's size only when the data came in the escape code;
        // from a file, it's how much to read.)
        const qint64 expected = bytesPerPixel ? qint64(cmd.width) * cmd.height * bytesPerPixel
                                : cmd.dataSize > 0 && cmd.medium == 'd' ? cmd.dataSize
                                : data.size() * 4LL;
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
        if (bytesPerPixel <= 2) {
            // Indexed: the palette index, and the alpha (opaque for f=8),
            // kept as they are; colourize() gives them the colours of the day.
            image = QImage(cmd.width, cmd.height, QImage::Format_Grayscale16);
            const auto *src = reinterpret_cast<const uchar *>(raw.constData());
            for (int y = 0; y < cmd.height; y++) {
                auto *dst = reinterpret_cast<quint16 *>(image.scanLine(y));
                const uchar *line = src + qint64(y) * cmd.width * bytesPerPixel;
                for (int x = 0; x < cmd.width; x++) {
                    const quint16 alpha = bytesPerPixel == 2 ? line[x * 2 + 1] : 255;
                    dst[x] = quint16(line[x * bytesPerPixel]) | quint16(alpha << 8);
                }
            }
            return {};
        }
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

QByteArray TerminalGraphics::read(const Command &cmd, const QByteArray &payload, QByteArray &data)
{
    switch (cmd.medium) {
    case 'd':
        data = payload;
        return {};
    case 'f':
    case 't': {
        // Checked before opening, symlinks followed: a regular file, and
        // nowhere sensitive.
        const QString path = QString::fromUtf8(payload);
        const QString real = QFileInfo(path).canonicalFilePath();
        const bool ok = QDir::isAbsolutePath(path) && !real.isEmpty() && !isSensitive(real)
                     && QFileInfo(real).isFile() && readFile(real, cmd.dataOffset, cmd.dataSize, data);
        if (cmd.medium == 't' && !real.isEmpty() && isTemporary(real))
            QFile::remove(real);
        return ok ? QByteArray() : kReadError;
    }
#ifdef Q_OS_UNIX
    case 's':
        return readSharedMemory(payload, cmd.dataOffset, cmd.dataSize, data) ? QByteArray() : kReadError;
#endif
    default:
        return "EINVAL:Unsupported transmission medium";
    }
}

TerminalGraphics::Result TerminalGraphics::transmit(const Command &cmd, const QByteArray &payload,
                                                    const Context &context)
{
    Result r;
    QByteArray data;
    QByteArray error = read(cmd, payload, data);
    QImage image;
    if (error.isEmpty())
        error = decode(cmd, data, image);
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
    Image stored;
    stored.frames = {Frame{image, 0}};
    stored.number = cmd.number;
    stored.serial = ++m_serial;
    m_images.insert(id, stored);

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

    const QRect full = it->frames.first().image.rect();
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
    case 'f': { // an image's frames, but the first
        const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
        const auto it = m_images.find(id);
        if (it != m_images.end() && it->frames.size() > 1) {
            it->frames.resize(1);
            it->current = 0;
            it->state = Image::Stopped;
            ++m_generation;
        }
        break;
    }
    default:
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

void TerminalGraphics::makeRoom(qint64 bytes, quint32 keep)
{
    qint64 used = 0;
    for (const Image &image : std::as_const(m_images))
        used += image.bytes();
    while (used + bytes > kQuota && m_images.size() > (m_images.contains(keep) ? 1 : 0)) {
        // The oldest no longer shown, else the oldest.
        quint32 victim = 0;
        quint64 oldest = ~0ULL;
        bool victimPlaced = true;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            const quint32 id = it.key();
            if (id == keep && keep != 0)
                continue;
            const bool placed = isPlaced(id);
            if ((victimPlaced && !placed) || (placed == victimPlaced && it->serial < oldest)) {
                victim = id;
                oldest = it->serial;
                victimPlaced = placed;
            }
        }
        used -= m_images.value(victim).bytes();
        dropImage(victim);
    }
}

bool TerminalGraphics::isPlaced(quint32 id) const
{
    return std::any_of(m_placements.cbegin(), m_placements.cend(),
                       [id](const GraphicsPlacement &p) { return p.imageId == id; });
}

qint64 TerminalGraphics::Image::bytes() const
{
    qint64 total = 0;
    for (const Frame &f : frames)
        total += f.image.sizeInBytes();
    return total;
}

QImage TerminalGraphics::image(quint32 id) const
{
    const auto it = m_images.constFind(id);
    if (it == m_images.constEnd() || it->frames.isEmpty())
        return {};
    return it->frames.at(qBound(0, it->current, int(it->frames.size()) - 1)).image;
}

QImage TerminalGraphics::colourize(const QImage &indexed, const QRgb *palette256)
{
    QImage out(indexed.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < indexed.height(); y++) {
        const auto *src = reinterpret_cast<const quint16 *>(indexed.constScanLine(y));
        auto *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < indexed.width(); x++) {
            const QRgb c = palette256[src[x] & 0xff];
            dst[x] = qPremultiply(qRgba(qRed(c), qGreen(c), qBlue(c), src[x] >> 8));
        }
    }
    return out;
}

qint64 TerminalGraphics::clock()
{
    static QElapsedTimer timer;
    if (!timer.isValid())
        timer.start();
    return timer.elapsed();
}

// An animation frame's data (a=f): a new frame, on a blank canvas or a copy
// of another frame (c), or an edit of one (r); the data goes at x, y.
TerminalGraphics::Result TerminalGraphics::frame(const Command &cmd, const QByteArray &payload)
{
    Result r;
    const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
    auto it = m_images.find(id);
    if (id == 0 || it == m_images.end()) {
        r.reply = reply(cmd, id, "ENOENT:No such image");
        return r;
    }
    if (isIndexed(it->frames.first().image) || cmd.format == 8 || cmd.format == 16) {
        r.reply = reply(cmd, id, "EINVAL:Indexed images can't be animated");
        return r;
    }
    QByteArray data;
    QByteArray error = read(cmd, payload, data);
    QImage patch;
    if (error.isEmpty())
        error = decode(cmd, data, patch);
    if (!error.isEmpty()) {
        r.reply = reply(cmd, id, error);
        return r;
    }
    const QRect canvas = it->frames.first().image.rect();
    const QRect area(cmd.x, cmd.y, patch.width(), patch.height());
    if (!canvas.contains(area)) {
        r.reply = reply(cmd, id, "EINVAL:Frame data is out of bounds");
        return r;
    }
    const int count = int(it->frames.size());
    if (cmd.keyR > quint32(count) || cmd.keyC > quint32(count)) {
        r.reply = reply(cmd, id, "ENOENT:No such frame");
        return r;
    }

    QImage target;
    if (cmd.keyR > 0) {
        target = it->frames.at(cmd.keyR - 1).image;
    } else if (cmd.keyC > 0) {
        target = it->frames.at(cmd.keyC - 1).image;
    } else {
        target = QImage(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        const quint32 rgba = cmd.keyY;   // the background, RGBA
        target.fill(QColor(int(rgba >> 24), int(rgba >> 16) & 0xff, int(rgba >> 8) & 0xff, int(rgba & 0xff)));
    }
    target = target.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&target);
        painter.setCompositionMode(cmd.keyX == 1 ? QPainter::CompositionMode_Source
                                                 : QPainter::CompositionMode_SourceOver);
        painter.drawImage(area.topLeft(), patch);
    }

    // The gap to the next: z > 0 milliseconds, z < 0 none; else as it was,
    // or 40 ms for a new frame.
    if (cmd.keyR > 0) {
        Frame &f = it->frames[cmd.keyR - 1];
        f.image = target;
        if (cmd.hasZ && cmd.z != 0)
            f.gap = qMax(0, cmd.z);
    } else {
        // Others make room for it, but it can't outgrow the quota itself.
        if (it->bytes() + target.sizeInBytes() > kQuota) {
            r.reply = reply(cmd, id, "ENOSPC:Too many frames");
            return r;
        }
        makeRoom(target.sizeInBytes(), id);
        it = m_images.find(id);
        it->frames.append(Frame{target, cmd.hasZ && cmd.z != 0 ? qMax(0, cmd.z) : 40});
    }
    ++m_generation;
    r.changed = true;
    r.reply = reply(cmd, id, "OK");
    return r;
}

// Controlling an animation (a=a): a frame's gap, the frame showing, playing.
TerminalGraphics::Result TerminalGraphics::animation(const Command &cmd)
{
    Result r;
    const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
    const auto it = m_images.find(id);
    if (id == 0 || it == m_images.end()) {
        r.reply = reply(cmd, id, "ENOENT:No such image");
        return r;
    }
    Image &image = *it;
    const int count = int(image.frames.size());
    if (cmd.keyR > quint32(count) || cmd.keyC > quint32(count)) {
        r.reply = reply(cmd, id, "ENOENT:No such frame");
        return r;
    }
    const qint64 now = clock();
    if (cmd.keyR > 0 && cmd.hasZ && cmd.z != 0)
        image.frames[cmd.keyR - 1].gap = qMax(0, cmd.z);
    if (cmd.keyC > 0) {
        image.current = int(cmd.keyC) - 1;
        image.shownAt = now;
    }
    switch (cmd.width) {    // s
    case 1:
        image.state = Image::Stopped;
        image.loopsDone = 0;
        break;
    case 2:
    case 3:
        if (image.state == Image::Stopped)
            image.shownAt = now;
        image.state = cmd.width == 2 ? Image::Loading : Image::Running;
        break;
    default:
        break;
    }
    if (cmd.height > 0)     // v
        image.loops = cmd.height;
    ++m_generation;
    r.changed = true;   // and no reply but errors, as kitty (icat doesn't read one)
    return r;
}

// Composing (a=c): a rectangle of frame r (at X, Y) onto frame c (at x, y).
TerminalGraphics::Result TerminalGraphics::compose(const Command &cmd)
{
    Result r;
    const quint32 id = cmd.hasId ? cmd.id : newestWithNumber(cmd.number);
    const auto it = m_images.find(id);
    if (id == 0 || it == m_images.end()) {
        r.reply = reply(cmd, id, "ENOENT:No such image");
        return r;
    }
    const int count = int(it->frames.size());
    if (cmd.keyR == 0 || cmd.keyC == 0 || cmd.keyR > quint32(count) || cmd.keyC > quint32(count)) {
        r.reply = reply(cmd, id, "ENOENT:No such frame");
        return r;
    }
    const QRect canvas = it->frames.first().image.rect();
    const int w = cmd.w > 0 ? cmd.w : canvas.width();
    const int h = cmd.h > 0 ? cmd.h : canvas.height();
    const QRect from(int(qMin<quint32>(cmd.keyX, INT_MAX / 2)), int(qMin<quint32>(cmd.keyY, INT_MAX / 2)), w, h);
    const QRect to(cmd.x, cmd.y, w, h);
    if (isIndexed(it->frames.first().image) || !canvas.contains(from) || !canvas.contains(to)
        || (cmd.keyR == cmd.keyC && from.intersects(to))) {
        r.reply = reply(cmd, id, "EINVAL:Bad rectangle or frames");
        return r;
    }
    const QImage source = it->frames.at(cmd.keyR - 1).image;
    QImage &target = it->frames[cmd.keyC - 1].image;
    target = target.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&target);
        painter.setCompositionMode(cmd.noCursorMove ? QPainter::CompositionMode_Source   // C=1
                                                    : QPainter::CompositionMode_SourceOver);
        painter.drawImage(to.topLeft(), source, from);
    }
    ++m_generation;
    r.changed = true;
    r.reply = reply(cmd, id, "OK");
    return r;
}

qint64 TerminalGraphics::animate(const QSet<quint32> &shown)
{
    const qint64 now = clock();
    qint64 next = -1;
    for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        Image &image = *it;
        const int count = int(image.frames.size());
        if (image.state == Image::Stopped || count < 2 || !shown.contains(it.key()))
            continue;
        bool waiting = false;
        int steps = 0;
        // On to the frame due now, past those with no gap; a frame or two
        // when drawn often, many when it hasn't been for a while.
        for (; steps < 100000; steps++) {
            const int gap = image.frames.at(image.current).gap;
            if (image.shownAt + gap > now)
                break;
            int following = image.current + 1;
            if (following >= count) {
                if (image.state == Image::Loading) {
                    waiting = true;   // for more frames
                    break;
                }
                ++image.loopsDone;
                if (image.loops > 1 && image.loopsDone >= image.loops - 1) {
                    image.state = Image::Stopped;
                    image.loopsDone = 0;
                    break;
                }
                following = 0;
            }
            image.shownAt += gap;
            image.current = following;
        }
        if (steps == 100000)
            image.shownAt = now;   // all gapless, or long unseen: start afresh
        const int gap = image.frames.at(image.current).gap;
        if (image.state != Image::Stopped && !waiting && gap > 0) {
            const qint64 due = qMax<qint64>(0, image.shownAt + gap - now);
            next = next < 0 ? due : qMin(next, due);
        }
    }
    return next;
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
