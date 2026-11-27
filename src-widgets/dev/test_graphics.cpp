/*
 * test-graphics — kitty's graphics protocol in qtermwidget (TerminalGraphics):
 * images sent in chunks, both as the spec has them and as kitten icat sends
 * them, and files and shared memory read as kitty reads them. Prints each
 * failure; exits 1 if there was any.
 */

#include "TerminalGraphics.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTextStream>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using Konsole::TerminalGraphics;

namespace {

int failures = 0;

void check(bool ok, const char *what, int line)
{
    if (!ok) {
        QTextStream(stderr) << "FAIL line " << line << ": " << what << "\n";
        ++failures;
    }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

TerminalGraphics::Result run(TerminalGraphics &g, const QByteArray &body)
{
    TerminalGraphics::Context c;
    c.lines = 24;
    c.columns = 80;
    c.cellSize = QSize(10, 20);
    return g.command(body, c);
}

// A w x h RGB image, every pixel the same.
QByteArray rgb(int w, int h)
{
    QByteArray d;
    for (int i = 0; i < w * h; ++i)
        d += QByteArray("\x10\x80\xf0", 3);
    return d;
}

// zlib's stream, as kitty wants for o=z (qCompress() puts a size first).
QByteArray zlib(const QByteArray &d) { return qCompress(d).mid(4); }

// Base64 cut into n chunks (kitty cuts on a multiple of 4).
QList<QByteArray> chunks(const QByteArray &data, int n)
{
    const QByteArray b = data.toBase64();
    const int step = ((b.size() / n) + 3) / 4 * 4;
    QList<QByteArray> out;
    for (int at = 0; at < b.size(); at += step)
        out << b.mid(at, step);
    return out;
}

void chunked()
{
    const QByteArray image = zlib(rgb(40, 30));

    // As kitten icat sends it: a=T (and q=2) on every chunk, no id.
    {
        TerminalGraphics g;
        const QList<QByteArray> parts = chunks(image, 3);
        CHECK(parts.size() == 3);
        TerminalGraphics::Result r = run(g, "a=T,q=2,f=24,o=z,m=1,s=40,v=30;" + parts[0]);
        CHECK(r.reply.isEmpty() && g.isEmpty());
        r = run(g, "a=T,q=2,m=1;" + parts[1]);
        CHECK(r.reply.isEmpty() && g.isEmpty());
        r = run(g, "a=T,q=2;" + parts[2]);
        CHECK(r.reply.isEmpty() && !g.isEmpty() && r.moveCursor);
    }
    // As the spec has it: only m after the first; answered, having an id.
    {
        TerminalGraphics g;
        const QList<QByteArray> parts = chunks(image, 3);
        run(g, "a=T,f=24,o=z,i=5,m=1,s=40,v=30;" + parts[0]);
        run(g, "m=1;" + parts[1]);
        const TerminalGraphics::Result r = run(g, "m=0;" + parts[2]);
        CHECK(r.reply == "\033_Gi=5;OK\033\\" && !g.isEmpty());
    }
    // A delete in the middle abandons it: what follows isn't taken for it.
    {
        TerminalGraphics g;
        const QList<QByteArray> parts = chunks(image, 3);
        run(g, "a=T,f=24,o=z,i=6,m=1,s=40,v=30;" + parts[0]);
        run(g, "a=d,d=A");
        const TerminalGraphics::Result r = run(g, "m=0;" + parts[2]);
        CHECK(g.isEmpty());
        Q_UNUSED(r);
    }
    // Not enough data: said so.
    {
        TerminalGraphics g;
        const TerminalGraphics::Result r = run(g, "a=t,f=24,i=7,s=40,v=30;" + rgb(40, 2).toBase64());
        CHECK(r.reply.startsWith("\033_Gi=7;ENODATA"));
    }
}

void media()
{
    const QByteArray pixel = rgb(1, 1);
    // A file whose S is more than it holds (kitten icat's queries give the
    // path's length): read as far as it goes, as kitty does.
    {
        const QString path = QDir::tempPath() + QStringLiteral("/pj-test-graphics-%1.rgb").arg(getpid());
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(pixel) == 3);
        f.close();
        TerminalGraphics g;
        const QByteArray b64 = path.toUtf8().toBase64();
        TerminalGraphics::Result r = run(g, "a=q,f=24,t=f,s=1,v=1,i=2,S=" + QByteArray::number(path.size()) + ";" + b64);
        CHECK(r.reply == "\033_Gi=2;OK\033\\");
        // An offset past its end: refused.
        r = run(g, "a=q,f=24,t=f,s=1,v=1,i=3,O=10;" + b64);
        CHECK(r.reply.startsWith("\033_Gi=3;EBADF"));
        QFile::remove(path);
    }
    // A temporary file kitty's way is removed once read.
    {
        const QString path = QDir::tempPath() + QStringLiteral("/tty-graphics-protocol-pj-%1").arg(getpid());
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(pixel) == 3);
        f.close();
        TerminalGraphics g;
        const TerminalGraphics::Result r = run(g, "a=q,f=24,t=t,s=1,v=1,i=4,S=99;" + path.toUtf8().toBase64());
        CHECK(r.reply == "\033_Gi=4;OK\033\\");
        CHECK(!QFile::exists(path));
    }
#ifdef Q_OS_UNIX
    // Shared memory with S more than it holds, as icat's query sends it.
    {
        const QByteArray name = "/pj-test-graphics-" + QByteArray::number(getpid());
        const int fd = shm_open(name.constData(), O_CREAT | O_RDWR | O_EXCL, 0600);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(ftruncate(fd, 3) == 0);
            CHECK(write(fd, pixel.constData(), 3) == 3);
            close(fd);
            TerminalGraphics g;
            const TerminalGraphics::Result r = run(g, "a=q,f=24,t=s,s=1,v=1,i=5,S=19;" + name.toBase64());
            CHECK(r.reply == "\033_Gi=5;OK\033\\");
            // Read once: gone.
            const int again = shm_open(name.constData(), O_RDONLY, 0);
            CHECK(again < 0);
            if (again >= 0) {
                close(again);
                shm_unlink(name.constData());
            }
        }
    }
#endif
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    chunked();
    media();
    if (failures == 0)
        QTextStream(stdout) << "test-graphics: all passed\n";
    return failures ? 1 : 0;
}
