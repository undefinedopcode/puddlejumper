/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class RexControl;
class RexSocket;

/*
 * The tar that a Rex fs block's streams carry (API revision 23): plain
 * POSIX/PAX, names relative with forward slashes, a directory before what it
 * holds; regular files, directories and symbolic links, with their
 * permission bits (no setuid, setgid or sticky) and modification times, and
 * no owners.
 */
namespace rextar {

struct Entry
{
    enum Type { File, Dir, Link };
    QString name;     // in the tar: relative, '/'-separated
    Type type = File;
    quint32 mode = 0644;
    qint64 size = 0;  // File
    qint64 mtime = 0; // seconds since the epoch
    QString link;     // Link: the target, as written
    QString source;   // a local file to send (File)
};

/* What sending these local paths would put in a tar: each a top-level entry
 * named by its last part, a directory with all it holds (links in it as
 * links; devices, FIFOs and sockets skipped). A path that is a link is
 * followed. Returns why not, or an empty string. */
QString collect(const QStringList &paths, QList<Entry> *entries, qint64 *bytes, int *roots);

/* An entry's header block(s): a PAX extended header first when the name,
 * link or size doesn't fit ustar's fields. */
QByteArray header(const Entry &e);
/* Zero bytes to the next 512-byte boundary after `size` bytes of a file. */
QByteArray padding(qint64 size);
/* The end marker: two zero blocks. */
QByteArray endMarker();

/*
 * Unpacks a tar arriving in pieces into a directory (empty, made for it),
 * treating it as hostile: absolute or ".." names, a name twice, hard links,
 * devices and other kinds, writing through a link the tar made, and an
 * entry before its directory are refused, as is a tar without its end
 * marker. Directories' own permissions and times are applied by finish(),
 * once everything is in.
 */
class Reader
{
public:
    explicit Reader(const QString &stage);
    ~Reader();

    /* Takes the next bytes; false once the tar is refused (error()). */
    bool feed(QByteArrayView bytes);
    /* After the last bytes: false if the tar wasn't whole. */
    bool finish();

    QString error() const { return m_error; }
    bool ended() const { return m_ended; }
    /* The top-level names, in order. */
    QStringList roots() const { return m_roots; }
    qint64 fileBytes() const { return m_fileBytes; }

private:
    bool fail(const QString &why);
    bool takeHeader();
    bool startEntry(const QString &name, char type, quint32 mode, qint64 size, qint64 mtime, const QString &link);
    void closeFile();

    enum class State { Header, Extended, Data, Pad, Done };

    QString m_stage;
    QByteArray m_buffer;
    State m_state = State::Header;
    qint64 m_left = 0;       // bytes of the current part still to come
    qint64 m_pad = 0;
    char m_extType = 0;      // 'x', 'g' or 'L': what the extended data is
    QByteArray m_ext;        // its bytes so far
    QHash<QString, QString> m_pax;  // the next entry's PAX records
    QString m_longName;      // a GNU long name for the next entry
    std::unique_ptr<QFile> m_file;
    qint64 m_fileMtime = 0;
    quint32 m_fileMode = 0;
    int m_zeroBlocks = 0;
    bool m_ended = false;
    QString m_error;
    QSet<QString> m_seen, m_dirs, m_links;
    QStringList m_roots;
    struct DirMeta
    {
        QString path;
        quint32 mode;
        qint64 mtime;
    };
    QList<DirMeta> m_dirMeta;
    qint64 m_fileBytes = 0;
};

/* Set a path's modification time (not following a link). */
void setMtime(const QString &path, qint64 mtime, bool link = false);

} // namespace rextar

/*
 * One copy to or from a Rex server, as `rex cp` makes it: a detached fs
 * block (com.superlogical.fs.fs) in the session, one of its two methods,
 * the tar over a rex.data.bytes.v1 stream, then the block closed.
 *
 * Upload: local files and directories into `remotePath` on the server, by
 * cp's rules there (into a directory that is there; one entry onto a new
 * name; a file replaces a file, a directory merges into a directory).
 *
 * Download: remote paths (patterns allowed; directories with all they
 * hold) into a local directory. Nothing there is replaced: a name that is
 * taken gets " (2)", " (3)"... The tar is unpacked beside it first and only
 * put in place once it has all arrived and checked out.
 *
 * Relative remote paths start at `workingDir` (empty: the server's home).
 */
class RexFileCopy : public QObject
{
    Q_OBJECT

public:
    RexFileCopy(RexControl *control, const QString &sessionId, QObject *parent = nullptr);
    ~RexFileCopy() override;

    void upload(const QStringList &localPaths, const QString &workingDir, const QString &remotePath);
    void download(const QStringList &remotePaths, const QString &workingDir, const QString &localDir);
    /* Stop: what was copied so far is thrown away (on either side). */
    void cancel();

    bool isUpload() const { return m_upload; }
    /* Where it went: the server's absolute path (upload), or the local
     * paths made (download). */
    QString destination() const { return m_destination; }
    QStringList placed() const { return m_placed; }
    int items() const { return m_roots; }

signals:
    void progress(qint64 done, qint64 total);
    /* Once. `error` is empty on success. */
    void finished(const QString &error);

private:
    void openBlock(const QString &workingDir);
    void begin();
    void connectStream(const QString &ticket);
    void pump();
    void onBinary(const QByteArray &data);
    void onDisconnected();
    void placeDownload();
    void done(const QString &error);
    void closeBlock();
    QString streamError(int code, const QString &reason) const;

    QPointer<RexControl> m_control;
    QString m_sessionId;
    QString m_blockId;
    bool m_upload = true;
    bool m_finished = false;
    bool m_cancelled = false;

    // Upload.
    QStringList m_localPaths;
    QString m_remotePath;
    QList<rextar::Entry> m_entries;
    int m_next = 0;               // the entry being sent
    std::unique_ptr<QFile> m_sending;
    qint64 m_sentOfFile = 0;
    bool m_tarSent = false;

    // Download.
    QStringList m_remotePaths;
    QString m_localDir;
    QString m_stage;
    std::unique_ptr<rextar::Reader> m_reader;
    bool m_serverEnded = false;   // its empty message came

    RexSocket *m_socket = nullptr;
    bool m_welcomed = false;
    qint64 m_total = 0, m_done = 0;
    int m_roots = 0;
    QString m_destination;
    QStringList m_placed;
    QElapsedTimer m_progressClock;
};
