/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexfilecopy.h"

#include "rexclient.h"
#include "rexsocket.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const QString kFsFlavor = QStringLiteral("com.superlogical.fs.fs");
const QString kBytesProtocol = QStringLiteral("rex.data.bytes.v1");
constexpr qsizetype kChunk = 64 * 1024;
constexpr qint64 kHighWater = 1 << 20;  // bytes queued before waiting
constexpr qint64 kMaxExtended = 1 << 20;

// ---- ustar fields -----------------------------------------------------------

void putOctal(char *field, int width, qint64 value)
{
    const QByteArray digits = QByteArray::number(value, 8).rightJustified(width - 1, '0');
    memcpy(field, digits.constData(), width - 1);
    field[width - 1] = 0;
}

bool fitsOctal(qint64 value, int width)
{
    return value >= 0 && QByteArray::number(value, 8).size() <= width - 1;
}

QByteArray block(const QByteArray &name, char type, quint32 mode, qint64 size, qint64 mtime,
                 const QByteArray &link)
{
    QByteArray h(512, '\0');
    char *p = h.data();
    memcpy(p, name.constData(), std::min<qsizetype>(name.size(), 100));
    putOctal(p + 100, 8, mode);
    putOctal(p + 108, 8, 0);
    putOctal(p + 116, 8, 0);
    putOctal(p + 124, 12, fitsOctal(size, 12) ? size : 0);
    putOctal(p + 136, 12, fitsOctal(mtime, 12) ? mtime : 0);
    p[156] = type;
    memcpy(p + 157, link.constData(), std::min<qsizetype>(link.size(), 100));
    memcpy(p + 257, "ustar\0" "00", 8);
    memset(p + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += uchar(p[i]);
    const QByteArray chk = QByteArray::number(sum, 8).rightJustified(6, '0');
    memcpy(p + 148, chk.constData(), 6);
    p[154] = 0;
    p[155] = ' ';
    return h;
}

QByteArray paxRecord(const QByteArray &key, const QByteArray &value)
{
    // "<length> key=value\n", the length counting itself.
    const qsizetype rest = 1 + key.size() + 1 + value.size() + 1;
    qsizetype len = rest + 1;
    while (QByteArray::number(len).size() + rest != len)
        len = QByteArray::number(len).size() + rest;
    return QByteArray::number(len) + ' ' + key + '=' + value + '\n';
}

qint64 parseNumber(const char *field, int width, bool *ok)
{
    *ok = true;
    if (uchar(field[0]) & 0x80) {
        // Base-256 (GNU), for values too big for octal.
        qint64 v = uchar(field[0]) & 0x3F;
        for (int i = 1; i < width; ++i) {
            if (v > (std::numeric_limits<qint64>::max() >> 8)) {
                *ok = false;
                return 0;
            }
            v = (v << 8) | uchar(field[i]);
        }
        return v;
    }
    QByteArray s(field, width);
    const qsizetype nul = s.indexOf('\0');
    if (nul >= 0)
        s.truncate(nul);
    s = s.trimmed();
    if (s.isEmpty())
        return 0;
    return s.toLongLong(ok, 8);
}

QString cString(const char *field, int width)
{
    const char *end = static_cast<const char *>(memchr(field, 0, width));
    return QString::fromUtf8(field, end ? end - field : width);
}

#ifdef Q_OS_UNIX
QString readLink(const QString &path)
{
    QByteArray buf(4096, Qt::Uninitialized);
    const ssize_t n = ::readlink(QFile::encodeName(path).constData(), buf.data(), buf.size());
    return n < 0 ? QString() : QFile::decodeName(buf.left(n));
}
#endif

bool setMode(const QString &path, quint32 mode)
{
#ifdef Q_OS_UNIX
    return ::chmod(QFile::encodeName(path).constData(), mode & 0777) == 0;
#else
    QFile::Permissions p;
    if (mode & 0400) p |= QFile::ReadOwner | QFile::ReadUser;
    if (mode & 0200) p |= QFile::WriteOwner | QFile::WriteUser;
    if (mode & 0100) p |= QFile::ExeOwner | QFile::ExeUser;
    if (mode & 0040) p |= QFile::ReadGroup;
    if (mode & 0020) p |= QFile::WriteGroup;
    if (mode & 0010) p |= QFile::ExeGroup;
    if (mode & 0004) p |= QFile::ReadOther;
    if (mode & 0002) p |= QFile::WriteOther;
    if (mode & 0001) p |= QFile::ExeOther;
    return QFile::setPermissions(path, p);
#endif
}

// A local file's or directory's details, the link itself if it is one
// (`follow` false).
struct Stat
{
    bool ok = false;
    rextar::Entry::Type type = rextar::Entry::File;
    bool special = false;  // a device, FIFO or socket
    quint32 mode = 0;
    qint64 size = 0, mtime = 0;
};

Stat statPath(const QString &path, bool follow)
{
    Stat st;
#ifdef Q_OS_UNIX
    struct stat sb;
    const QByteArray p = QFile::encodeName(path);
    if ((follow ? ::stat(p.constData(), &sb) : ::lstat(p.constData(), &sb)) != 0)
        return st;
    st.ok = true;
    st.mode = sb.st_mode & 0777;
    st.size = sb.st_size;
    st.mtime = sb.st_mtime;
    if (S_ISDIR(sb.st_mode))
        st.type = rextar::Entry::Dir;
    else if (S_ISLNK(sb.st_mode))
        st.type = rextar::Entry::Link;
    else if (!S_ISREG(sb.st_mode))
        st.special = true;
#else
    const QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink())
        return st;
    st.ok = true;
    if (!follow && fi.isSymLink()) {
        st.type = rextar::Entry::Link;  // left out: no links sent from here
        return st;
    }
    st.type = fi.isDir() ? rextar::Entry::Dir : rextar::Entry::File;
    st.special = !fi.isDir() && !fi.isFile();
    const QFile::Permissions p = fi.permissions();
    st.mode = (p & QFile::ReadOwner ? 0444 : 0) | (p & QFile::WriteOwner ? 0200 : 0)
              | (st.type == rextar::Entry::Dir || p & QFile::ExeOwner ? 0111 : 0);
    st.size = fi.size();
    st.mtime = fi.lastModified().toSecsSinceEpoch();
#endif
    return st;
}

QString newRef(const char *kind)
{
    return QString::fromLatin1(kind) + QLatin1Char(':') + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString freeName(const QDir &dir, const QString &name)
{
    auto taken = [&](const QString &n) {
        const QFileInfo fi(dir.filePath(n));
        return fi.exists() || fi.isSymLink();
    };
    if (!taken(name))
        return name;
    // "name (2).ext": the extension kept, a leading dot not taken for one.
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    for (int i = 2;; ++i) {
        const QString n = QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(ext);
        if (!taken(n))
            return n;
    }
}

} // namespace

// ---- rextar -----------------------------------------------------------------

namespace rextar {

QString collect(const QStringList &paths, QList<Entry> *entries, qint64 *bytes, int *roots)
{
    entries->clear();
    *bytes = 0;
    *roots = 0;
    QHash<QString, QString> byName;
    for (const QString &given : paths) {
        const QString path = QDir::cleanPath(QFileInfo(given).absoluteFilePath());
        const QString name = QFileInfo(path).fileName();
        if (name.isEmpty())
            return QObject::tr("%1: can't be copied as a whole").arg(given);
        const Stat st = statPath(path, true);  // a named link is followed
        if (!st.ok)
            return QObject::tr("%1: no such file or directory").arg(given);
        if (st.special)
            return QObject::tr("%1: is not a file or a directory, so it can't be copied").arg(given);
        if (byName.contains(name))
            return QObject::tr("%1: has the same name as %2, so one would replace the other")
                .arg(given, byName.value(name));
        byName.insert(name, given);
        ++*roots;

        std::function<QString(const QString &, const QString &, const Stat &)> add =
            [&](const QString &p, const QString &rel, const Stat &s) -> QString {
            Entry e;
            e.name = rel;
            e.type = s.type;
            e.mode = s.mode;
            e.mtime = s.mtime;
            if (s.type == Entry::File) {
                e.size = s.size;
                e.source = p;
                *bytes += s.size;
            } else if (s.type == Entry::Link) {
#ifdef Q_OS_UNIX
                e.link = readLink(p);
                if (e.link.isEmpty())
                    return QObject::tr("%1: the link can't be read").arg(p);
#else
                return QString();  // Windows links aren't sent
#endif
            }
            entries->append(e);
            if (s.type != Entry::Dir)
                return QString();
            QDir dir(p);
            if (!dir.isReadable())
                return QObject::tr("%1: can't be read").arg(p);
            const QStringList names =
                dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QString &n : names) {
                const QString child = dir.filePath(n);
                const Stat cs = statPath(child, false);
                if (!cs.ok || cs.special)
                    continue;  // gone, or a device, FIFO or socket: skipped
                if (QString err = add(child, rel + QLatin1Char('/') + n, cs); !err.isEmpty())
                    return err;
            }
            return QString();
        };
        if (QString err = add(path, name, st); !err.isEmpty())
            return err;
    }
    return QString();
}

QByteArray header(const Entry &e)
{
    QByteArray name = e.name.toUtf8();
    if (e.type == Entry::Dir)
        name += '/';
    const QByteArray link = e.link.toUtf8();
    const char type = e.type == Entry::Dir ? '5' : e.type == Entry::Link ? '2' : '0';
    const qint64 size = e.type == Entry::File ? e.size : 0;
    QByteArray pax;
    if (name.size() > 100)
        pax += paxRecord("path", name);
    if (link.size() > 100)
        pax += paxRecord("linkpath", link);
    if (!fitsOctal(size, 12))
        pax += paxRecord("size", QByteArray::number(size));
    if (!fitsOctal(e.mtime, 12))
        pax += paxRecord("mtime", QByteArray::number(e.mtime));
    QByteArray out;
    if (!pax.isEmpty()) {
        out += block("PaxHeaders/" + name.right(80), 'x', 0644, pax.size(), e.mtime, QByteArray());
        out += pax + padding(pax.size());
    }
    out += block(name, type, e.mode & 0777, size, e.mtime, link);
    return out;
}

QByteArray padding(qint64 size)
{
    return QByteArray(int((512 - size % 512) % 512), '\0');
}

QByteArray endMarker()
{
    return QByteArray(1024, '\0');
}

void setMtime(const QString &path, qint64 mtime, bool link)
{
#ifdef Q_OS_UNIX
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = time_t(mtime);
    times[0].tv_nsec = times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, link ? AT_SYMLINK_NOFOLLOW : 0);
#else
    if (link)
        return;
    QFile f(path);
    if (f.open(QIODevice::ReadWrite | QIODevice::ExistingOnly))
        f.setFileTime(QDateTime::fromSecsSinceEpoch(mtime), QFileDevice::FileModificationTime);
#endif
}

Reader::Reader(const QString &stage)
    : m_stage(stage)
{
}

Reader::~Reader()
{
    closeFile();
}

bool Reader::fail(const QString &why)
{
    if (m_error.isEmpty())
        m_error = why;
    closeFile();
    m_state = State::Done;
    return false;
}

void Reader::closeFile()
{
    if (!m_file)
        return;
    const QString path = m_file->fileName();
    m_file->close();
    m_file.reset();
    setMode(path, m_fileMode);
    setMtime(path, m_fileMtime);
}

bool Reader::feed(QByteArrayView bytes)
{
    if (!m_error.isEmpty())
        return false;
    m_buffer += bytes;
    qsizetype at = 0;
    while (at < m_buffer.size()) {
        const qsizetype avail = m_buffer.size() - at;
        switch (m_state) {
        case State::Done:
            at = m_buffer.size();  // anything after the end marker
            break;
        case State::Header:
            if (avail < 512) {
                m_buffer.remove(0, at);
                return true;
            }
            {
                const QByteArray h = m_buffer.mid(at, 512);
                at += 512;
                m_buffer.remove(0, at);
                at = 0;
                // takeHeader works on m_ext as scratch: put the header there.
                m_ext = h;
                if (!takeHeader())
                    return false;
            }
            break;
        case State::Extended: {
            const qsizetype n = std::min<qint64>(avail, m_left);
            m_ext += QByteArrayView(m_buffer).sliced(at, n);
            at += n;
            m_left -= n;
            if (m_left == 0) {
                if (m_extType == 'x') {
                    // "<len> key=value\n" records.
                    qsizetype p = 0;
                    while (p < m_ext.size()) {
                        const qsizetype sp = m_ext.indexOf(' ', p);
                        if (sp < 0)
                            return fail(QObject::tr("a malformed PAX header"));
                        bool ok = false;
                        const qsizetype len = m_ext.mid(p, sp - p).toLongLong(&ok);
                        if (!ok || len <= 0 || p + len > m_ext.size() || m_ext.at(p + len - 1) != '\n')
                            return fail(QObject::tr("a malformed PAX header"));
                        const QByteArray rec = m_ext.mid(sp + 1, p + len - sp - 2);
                        const qsizetype eq = rec.indexOf('=');
                        if (eq < 0)
                            return fail(QObject::tr("a malformed PAX header"));
                        m_pax.insert(QString::fromUtf8(rec.left(eq)), QString::fromUtf8(rec.mid(eq + 1)));
                        p += len;
                    }
                } else if (m_extType == 'L') {
                    m_longName = QString::fromUtf8(m_ext.left(m_ext.indexOf('\0') >= 0 ? m_ext.indexOf('\0') : m_ext.size()));
                }
                m_ext.clear();
                m_state = m_pad ? State::Pad : State::Header;
            }
            break;
        }
        case State::Data: {
            const qsizetype n = std::min<qint64>(avail, m_left);
            if (m_file && m_file->write(m_buffer.constData() + at, n) != n)
                return fail(QObject::tr("%1: %2").arg(m_file->fileName(), m_file->errorString()));
            at += n;
            m_left -= n;
            if (m_left == 0) {
                closeFile();
                m_state = m_pad ? State::Pad : State::Header;
            }
            break;
        }
        case State::Pad: {
            const qsizetype n = std::min<qint64>(avail, m_pad);
            at += n;
            m_pad -= n;
            if (m_pad == 0)
                m_state = State::Header;
            break;
        }
        }
    }
    m_buffer.clear();
    return m_error.isEmpty();
}

bool Reader::takeHeader()
{
    const QByteArray h = std::exchange(m_ext, QByteArray());
    const char *p = h.constData();
    if (std::all_of(h.cbegin(), h.cend(), [](char c) { return c == 0; })) {
        if (++m_zeroBlocks == 2) {
            m_ended = true;
            m_state = State::Done;
        }
        return true;
    }
    if (m_zeroBlocks)
        return fail(QObject::tr("a malformed tar: an entry after a zero block"));
    // The checksum: the header's bytes with its own field as spaces.
    bool ok = false;
    const qint64 want = parseNumber(p + 148, 8, &ok);
    unsigned usum = 0;
    int ssum = 0;
    for (int i = 0; i < 512; ++i) {
        const char c = (i >= 148 && i < 156) ? ' ' : p[i];
        usum += uchar(c);
        ssum += static_cast<signed char>(c);
    }
    if (!ok || (want != qint64(usum) && want != qint64(ssum)))
        return fail(QObject::tr("a malformed tar: a header's checksum is wrong"));
    const char type = p[156];
    qint64 size = parseNumber(p + 124, 12, &ok);
    if (!ok || size < 0)
        return fail(QObject::tr("a malformed tar: a bad size"));
    if (type == 'x' || type == 'g' || type == 'L') {
        if (size > kMaxExtended)
            return fail(QObject::tr("a malformed tar: an extended header too large"));
        m_extType = type;
        m_left = size;
        m_pad = (512 - size % 512) % 512;
        m_ext.clear();
        if (type == 'g')
            m_extType = 'g';
        m_state = size ? State::Extended : (m_pad ? State::Pad : State::Header);
        return true;
    }
    QString name = cString(p, 100);
    if (memcmp(p + 257, "ustar", 5) == 0) {
        const QString prefix = cString(p + 345, 155);
        if (!prefix.isEmpty())
            name = prefix + QLatin1Char('/') + name;
    }
    if (!m_longName.isEmpty())
        name = std::exchange(m_longName, QString());
    QString link = cString(p + 157, 100);
    qint64 mtime = parseNumber(p + 136, 12, &ok);
    const quint32 mode = quint32(parseNumber(p + 100, 8, &ok)) & 0777;
    if (m_pax.contains(QStringLiteral("path")))
        name = m_pax.value(QStringLiteral("path"));
    if (m_pax.contains(QStringLiteral("linkpath")))
        link = m_pax.value(QStringLiteral("linkpath"));
    if (m_pax.contains(QStringLiteral("size"))) {
        size = m_pax.value(QStringLiteral("size")).toLongLong(&ok);
        if (!ok || size < 0)
            return fail(QObject::tr("a malformed tar: a bad size"));
    }
    if (m_pax.contains(QStringLiteral("mtime")))
        mtime = qint64(m_pax.value(QStringLiteral("mtime")).toDouble());
    m_pax.clear();
    return startEntry(name, type, mode, size, mtime, link);
}

bool Reader::startEntry(const QString &rawName, char type, quint32 mode, qint64 size, qint64 mtime,
                        const QString &link)
{
    QString name = rawName;
    while (name.endsWith(QLatin1Char('/')))
        name.chop(1);
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QChar(0)))
        return fail(QObject::tr("the tar names “%1”").arg(rawName));
    const QStringList parts = name.split(QLatin1Char('/'));
    for (const QString &part : parts)
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return fail(QObject::tr("the tar names “%1”").arg(rawName));
    if (m_seen.contains(name))
        return fail(QObject::tr("the tar names “%1” twice").arg(name));
    m_seen.insert(name);
    for (int i = 1; i < parts.size(); ++i) {
        const QString prefix = parts.mid(0, i).join(QLatin1Char('/'));
        if (m_links.contains(prefix))
            return fail(QObject::tr("“%1” would be written through the link “%2”").arg(name, prefix));
        if (!m_dirs.contains(prefix))
            return fail(QObject::tr("“%1” comes before its directory").arg(name));
    }
    const QString path = m_stage + QLatin1Char('/') + name;
    m_left = size;
    m_pad = (512 - size % 512) % 512;
    switch (type) {
    case '0':
    case '\0':
    case '7': {
        m_file = std::make_unique<QFile>(path);
        if (!m_file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            const QString err = m_file->errorString();
            m_file.reset();
            return fail(QObject::tr("%1: %2").arg(name, err));
        }
        m_fileMode = mode;
        m_fileMtime = mtime;
        m_fileBytes += size;
        break;
    }
    case '5':
        if (!QDir().mkdir(path))
            return fail(QObject::tr("%1: can't make the directory").arg(name));
        m_dirs.insert(name);
        m_dirMeta.append({path, mode, mtime});
        break;
    case '2':
        if (link.isEmpty())
            return fail(QObject::tr("the link “%1” leads nowhere").arg(name));
#ifdef Q_OS_UNIX
        if (::symlink(QFile::encodeName(link).constData(), QFile::encodeName(path).constData()) != 0)
            return fail(QObject::tr("%1: can't make the link").arg(name));
        setMtime(path, mtime, true);
#endif
        // Elsewhere links are left out.
        m_links.insert(name);
        break;
    case '1':
        return fail(QObject::tr("the tar holds a hard link, “%1”").arg(name));
    default:
        return fail(QObject::tr("the tar holds “%1”, which is not a file, directory or link").arg(name));
    }
    if (parts.size() == 1)
        m_roots.append(name);
    if (m_left > 0)
        m_state = State::Data;  // a directory's or link's bytes are dropped
    else if (m_file)
        closeFile();
    if (m_state != State::Data)
        m_state = m_pad ? State::Pad : State::Header;
    return true;
}

bool Reader::finish()
{
    if (!m_error.isEmpty())
        return false;
    if (!m_ended)
        return fail(QObject::tr("the tar was cut short"));
    // Directories' own permissions and times, deepest first, now that
    // nothing more is written into them.
    std::sort(m_dirMeta.begin(), m_dirMeta.end(),
              [](const DirMeta &a, const DirMeta &b) { return a.path.count(QLatin1Char('/')) > b.path.count(QLatin1Char('/')); });
    for (const DirMeta &d : std::as_const(m_dirMeta)) {
        setMtime(d.path, d.mtime);
        setMode(d.path, d.mode | 0200);  // writable by its owner, so it can be moved and removed
    }
    return true;
}

} // namespace rextar

// ---- RexFileCopy ------------------------------------------------------------

RexFileCopy::RexFileCopy(RexControl *control, const QString &sessionId, QObject *parent)
    : QObject(parent)
    , m_control(control)
    , m_sessionId(sessionId)
{
}

RexFileCopy::~RexFileCopy()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
    m_sending.reset();
    m_reader.reset();
    if (!m_stage.isEmpty())
        QDir(m_stage).removeRecursively();
    if (!m_finished)
        closeBlock();
}

void RexFileCopy::upload(const QStringList &localPaths, const QString &workingDir, const QString &remotePath)
{
    m_upload = true;
    m_localPaths = localPaths;
    m_remotePath = remotePath;
    qint64 bytes = 0;
    if (const QString err = rextar::collect(localPaths, &m_entries, &bytes, &m_roots); !err.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, err]() { done(err); }, Qt::QueuedConnection);
        return;
    }
    m_total = bytes;
    openBlock(workingDir);
}

void RexFileCopy::download(const QStringList &remotePaths, const QString &workingDir, const QString &localDir)
{
    m_upload = false;
    m_remotePaths = remotePaths;
    m_localDir = localDir;
    m_destination = localDir;
    openBlock(workingDir);
}

void RexFileCopy::cancel()
{
    if (m_finished)
        return;
    m_cancelled = true;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
    done(tr("cancelled"));
}

void RexFileCopy::openBlock(const QString &workingDir)
{
    if (!m_control) {
        QMetaObject::invokeMethod(this, [this]() { done(tr("not connected to the server")); }, Qt::QueuedConnection);
        return;
    }
    QJsonObject options;
    if (!workingDir.isEmpty())
        options.insert(QStringLiteral("working_dir"), workingDir);
    QPointer<RexFileCopy> self(this);
    m_control->requestOnce(QStringLiteral("session.new_block"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("flavor"), kFsFlavor},
                                   {QStringLiteral("options"), options},
                                   {QStringLiteral("label"), QStringLiteral("PuddleJumper copy")}},
                       [self, workingDir](const QJsonObject &reply, const QJsonObject &error) {
                           if (!self || self->m_finished)
                               return;
                           if (!error.isEmpty()) {
                               QString msg = error.value(QLatin1String("message")).toString();
                               if (msg.contains(QLatin1String("flavor")))
                                   msg = tr("the server can't copy files (it has no fs block: an older "
                                            "server, or one started with --no-fs)");
                               else if (msg.contains(QLatin1String("working_dir")))
                                   msg = tr("%1 isn't a directory on the server").arg(workingDir);
                               self->done(msg);
                               return;
                           }
                           self->m_blockId = reply.value(QLatin1String("block_id")).toString();
                           self->begin();
                       });
}

void RexFileCopy::begin()
{
    QPointer<RexFileCopy> self(this);
    auto method = [this](const QString &name, const QJsonObject &args, RexControl::Reply reply) {
        m_control->request(QStringLiteral("block.method"),
                           QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                       {QStringLiteral("block_id"), m_blockId},
                                       {QStringLiteral("method"), name},
                                       {QStringLiteral("args"), args}},
                           std::move(reply));
    };
    if (!m_control) {
        done(tr("not connected to the server"));
        return;
    }
    if (m_upload) {
        QJsonObject args{{QStringLiteral("ref"), newRef("upload")},
                         {QStringLiteral("roots"), m_roots},
                         {QStringLiteral("entries"), qint64(m_entries.size())},
                         {QStringLiteral("size"), m_total}};
        if (!m_remotePath.isEmpty())
            args.insert(QStringLiteral("path"), m_remotePath);
        method(QStringLiteral("upload_begin"), args, [self](const QJsonObject &reply, const QJsonObject &error) {
            if (!self || self->m_finished)
                return;
            if (!error.isEmpty()) {
                self->done(error.value(QLatin1String("message")).toString());
                return;
            }
            self->m_destination = reply.value(QLatin1String("path")).toString();
            self->connectStream(reply.value(QLatin1String("ticket")).toString());
        });
        return;
    }
    QJsonArray paths;
    for (const QString &p : std::as_const(m_remotePaths))
        paths.append(p);
    method(QStringLiteral("download_begin"),
           QJsonObject{{QStringLiteral("ref"), newRef("download")},
                       {QStringLiteral("paths"), paths},
                       {QStringLiteral("recursive"), true}},
           [self](const QJsonObject &reply, const QJsonObject &error) {
               if (!self || self->m_finished)
                   return;
               if (!error.isEmpty()) {
                   self->done(error.value(QLatin1String("message")).toString());
                   return;
               }
               self->m_total = qint64(reply.value(QLatin1String("size")).toDouble());
               self->m_roots = reply.value(QLatin1String("roots")).toInt();
               // Unpacked beside where it goes, so it can be moved in whole.
               QDir local(self->m_localDir);
               const QString stage = QStringLiteral(".puddlejumper-copy-") + QUuid::createUuid().toString(QUuid::Id128);
               if (!local.exists() || !local.mkdir(stage)) {
                   self->done(tr("%1: can't write there").arg(self->m_localDir));
                   return;
               }
               self->m_stage = local.filePath(stage);
               self->m_reader = std::make_unique<rextar::Reader>(self->m_stage);
               self->connectStream(reply.value(QLatin1String("ticket")).toString());
           });
}

void RexFileCopy::connectStream(const QString &ticketText)
{
    const QByteArray ticket = QByteArray::fromBase64(ticketText.toLatin1(),
                                                     QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (ticket.size() != 16 || !m_control) {
        done(tr("the server gave no ticket for the copy"));
        return;
    }
    m_progressClock.start();
    m_socket = m_control->newSocket(QStringLiteral("/api/data"), kBytesProtocol, 16 << 20, this);
    connect(m_socket, &RexSocket::connected, this, [this, ticket]() { m_socket->sendBinaryMessage('\x01' + ticket); });
    connect(m_socket, &RexSocket::binaryMessageReceived, this, &RexFileCopy::onBinary);
    connect(m_socket, &RexSocket::bytesWritten, this, &RexFileCopy::pump);
    connect(m_socket, &RexSocket::disconnected, this, &RexFileCopy::onDisconnected);
}

void RexFileCopy::onBinary(const QByteArray &data)
{
    if (m_finished)
        return;
    if (!m_welcomed) {
        if (data.size() >= 2 && quint8(data[0]) == 0x82) {
            done(tr("the server refused the copy's stream (reason %1)").arg(quint8(data[1])));
            m_socket->abort();
            return;
        }
        if (data.size() != 32 || quint8(data[0]) != 0x81) {
            done(tr("the server's stream didn't start as it should"));
            m_socket->abort();
            return;
        }
        m_welcomed = true;
        if (m_upload)
            pump();
        else
            m_socket->sendBinaryMessage(QByteArray());  // nothing to send: the end at once
        return;
    }
    if (m_upload)
        return;  // the server sends nothing but the end of its output
    if (data.isEmpty()) {
        m_serverEnded = true;
        return;
    }
    if (!m_reader->feed(data)) {
        const QString err = tr("the server sent something this client won't unpack: %1").arg(m_reader->error());
        m_socket->disconnect(this);
        m_socket->abort();
        done(err);
        return;
    }
    m_done += data.size();
    if (m_progressClock.elapsed() > 100) {
        m_progressClock.restart();
        emit progress(std::min(m_done, m_total), m_total);
    }
}

void RexFileCopy::pump()
{
    if (!m_upload || !m_welcomed || m_finished || m_tarSent || !m_socket)
        return;
    while (m_socket->bytesToWrite() < kHighWater && !m_tarSent) {
        QByteArray msg;
        msg.reserve(kChunk + 2048);
        while (msg.size() < kChunk) {
            if (m_sending) {
                const rextar::Entry &e = m_entries.at(m_next);
                const qint64 want = std::min<qint64>(kChunk - msg.size(), e.size - m_sentOfFile);
                const qsizetype before = msg.size();
                msg.resize(before + want);
                const qint64 got = want ? m_sending->read(msg.data() + before, want) : 0;
                if (got < want) {
                    const QString err = got < 0 ? tr("%1: %2").arg(e.source, m_sending->errorString())
                                                : tr("%1 shrank while it was copied").arg(e.source);
                    m_socket->disconnect(this);
                    m_socket->abort();  // the server throws away what it has
                    done(err);
                    return;
                }
                m_sentOfFile += got;
                m_done += got;
                if (m_sentOfFile == e.size) {
                    msg += rextar::padding(e.size);
                    m_sending.reset();
                    ++m_next;
                }
                continue;
            }
            if (m_next < m_entries.size()) {
                const rextar::Entry &e = m_entries.at(m_next);
                msg += rextar::header(e);
                if (e.type == rextar::Entry::File) {
                    m_sending = std::make_unique<QFile>(e.source);
                    if (!m_sending->open(QIODevice::ReadOnly)) {
                        const QString err = tr("%1: %2").arg(e.source, m_sending->errorString());
                        m_socket->disconnect(this);
                        m_socket->abort();
                        done(err);
                        return;
                    }
                    m_sentOfFile = 0;
                    if (e.size == 0) {
                        m_sending.reset();
                        ++m_next;
                    }
                } else {
                    ++m_next;
                }
                continue;
            }
            msg += rextar::endMarker();
            m_tarSent = true;
            break;
        }
        m_socket->sendBinaryMessage(msg);
        if (m_tarSent)
            m_socket->sendBinaryMessage(QByteArray());  // the end of this side's output
    }
    if (m_progressClock.elapsed() > 100 || m_tarSent) {
        m_progressClock.restart();
        emit progress(m_done, m_total);
    }
}

QString RexFileCopy::streamError(int code, const QString &reason) const
{
    if (code == 4006)
        return m_upload ? tr("the server refused the copy: a file would have replaced a directory or a "
                             "directory a file, or the copy broke one of the server's rules (its log says "
                             "which); nothing was changed")
                        : tr("the server stopped the copy: %1").arg(reason);
    if (code == 1011)
        return m_upload ? tr("the server couldn't write the files (its log says why); nothing was changed")
                        : tr("the server couldn't read the files (its log says why)");
    if (reason.isEmpty())
        return tr("the connection was lost (code %1)").arg(code);
    return tr("the connection was lost: %1").arg(reason);
}

void RexFileCopy::onDisconnected()
{
    if (m_finished)
        return;
    const int code = m_socket->closeCode();
    const QString reason = m_socket->failureReason();
    m_socket->deleteLater();
    m_socket = nullptr;
    if (m_upload) {
        if (code == 1000 && m_tarSent)
            done(QString());
        else
            done(streamError(code, reason));
        return;
    }
    if (!m_serverEnded) {
        done(streamError(code == 1000 ? 1006 : code, reason));
        return;
    }
    if (!m_reader->finish()) {
        done(tr("the server sent something this client won't unpack: %1").arg(m_reader->error()));
        return;
    }
    placeDownload();
}

void RexFileCopy::placeDownload()
{
    const QDir local(m_localDir);
    for (const QString &root : m_reader->roots()) {
        const QString name = freeName(local, root);
        if (!QDir().rename(m_stage + QLatin1Char('/') + root, local.filePath(name))) {
            done(tr("%1: can't put it in place").arg(local.filePath(name)));
            return;
        }
        m_placed.append(local.filePath(name));
    }
    emit progress(m_total, m_total);
    done(QString());
}

void RexFileCopy::done(const QString &error)
{
    if (m_finished)
        return;
    m_finished = true;
    m_sending.reset();
    m_reader.reset();
    if (!m_stage.isEmpty()) {
        QDir(m_stage).removeRecursively();
        m_stage.clear();
    }
    closeBlock();
    emit finished(error);
}

void RexFileCopy::closeBlock()
{
    if (m_blockId.isEmpty() || !m_control)
        return;
    m_control->request(QStringLiteral("block.close"),
                       QJsonObject{{QStringLiteral("session_id"), m_sessionId},
                                   {QStringLiteral("block_id"), std::exchange(m_blockId, QString())}});
}
