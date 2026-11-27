/*
 * test-clipboard — programs' clipboard writes in qtermwidget's emulator:
 * OSC 52 (xterm's) and OSC 5522 (kitty's), against the clipboard of the
 * platform it runs on (offscreen by default: in-process, no selection).
 * Prints each failure; exits 1 if there was any.
 */

#include "qtermprobe.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTextStream>

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

QByteArray b64(const QByteArray &s) { return s.toBase64(); }

struct Probe : QtermProbe
{
    Probe() : QtermProbe(80, 24)
    {
        connect(this, &Emulation::sendData, this, [this](const char *d, int n) { replies += QByteArray(d, n); });
        connect(this, &Emulation::clipboardWritten, this,
                [this](bool primary, qint64 characters, const QStringList &types) {
                    ++writes;
                    lastPrimary = primary;
                    lastCharacters = characters;
                    lastTypes = types;
                });
    }
    QByteArray take()
    {
        QByteArray r = replies;
        replies.clear();
        return r;
    }
    QByteArray replies;
    int writes = 0;
    bool lastPrimary = false;
    qint64 lastCharacters = -2;
    QStringList lastTypes;
};

QString clip() { return QGuiApplication::clipboard()->text(); }
void setClip(const QString &s) { QGuiApplication::clipboard()->setText(s); }

QByteArray k(const QByteArray &meta, const QByteArray &payload = {})
{
    return "\033]5522;" + meta + (payload.isEmpty() ? QByteArray() : ";" + payload) + "\033\\";
}

void osc52()
{
    Probe p;
    setClip(QStringLiteral("before"));
    // BEL-terminated, and the text around it printed as usual.
    p.feed("A\033]52;c;" + b64("hello") + "\aB");
    CHECK(clip() == QLatin1String("hello"));
    CHECK(p.writes == 1 && !p.lastPrimary && p.lastCharacters == 5);
    CHECK(QtermProbeLines::screenLines(p.showing()).value(0) == QLatin1String("AB"));
    // ST-terminated, not ASCII, and longer than the token buffer (16384).
    const QString big = QString(QStringLiteral("ünïcødé ✓ ")).repeated(20000);
    p.feed("\033]52;c;" + b64(big.toUtf8()) + "\033\\");
    CHECK(clip() == big);
    CHECK(p.lastCharacters == big.size());
    // No target named: the clipboard.
    p.feed("\033]52;;" + b64("none named") + "\a");
    CHECK(clip() == QLatin1String("none named"));
    // Bad base64: nothing.
    p.feed("\033]52;c;!!notbase64!!\a");
    CHECK(clip() == QLatin1String("none named"));
    // Only the primary selection, which offscreen hasn't: nothing.
    if (!QGuiApplication::clipboard()->supportsSelection()) {
        const int before = p.writes;
        p.feed("\033]52;p;" + b64("primary") + "\a");
        CHECK(clip() == QLatin1String("none named") && p.writes == before);
    }
    // The primary selection, where there is one.
    if (QGuiApplication::clipboard()->supportsSelection()) {
        p.feed("\033]52;p;" + b64("to primary") + "\a");
        p.settle();
        CHECK(QGuiApplication::clipboard()->text(QClipboard::Selection) == QLatin1String("to primary"));
        CHECK(clip() == QLatin1String("none named") && p.lastPrimary);
    }
    // Empty: cleared.
    p.feed("\033]52;c;\a");
    CHECK(clip().isEmpty() && p.lastCharacters == 0 && p.lastTypes.isEmpty());
    CHECK(p.take().isEmpty());

    // Reads: answered as empty, never with the clipboard.
    setClip(QStringLiteral("secret"));
    p.feed("\033]52;c;?\a");
    CHECK(p.take() == "\033]52;c;\033\\");
    p.setAnswerQueries(false); // behind tmux, tmux answers
    p.feed("\033]52;c;?\a");
    CHECK(p.take().isEmpty());
    p.setAnswerQueries(true);

    // Off (a Rex pane): nothing at all.
    p.setClipboardWrites(false);
    p.feed("\033]52;c;" + b64("off") + "\a");
    CHECK(clip() == QLatin1String("secret"));
    p.setClipboardWrites(true);

    // A title still works after all that.
    p.feed("\033]2;title\a");
    p.settle();
    CHECK(p.titles.value(2) == QLatin1String("title"));
}

void osc5522()
{
    Probe p;
    setClip(QStringLiteral("before"));
    // Text in two chunks, a PNG, and an alias; the id comes back.
    p.feed(k("type=write:id=w1"));
    p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("hello ")));
    p.feed(k("type=wdata:mime=" + b64("image/png"), b64("\x89PNG fake")));
    p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("world")));
    p.feed(k("type=walias:mime=" + b64("text/plain"), b64("text/x-moz-text UTF8_STRING")));
    CHECK(p.take().isEmpty() && clip() == QLatin1String("before"));
    p.feed(k("type=wdata"));
    CHECK(p.take() == k("type=write:status=DONE:id=w1"));
    CHECK(clip() == QLatin1String("hello world"));
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    CHECK(md && md->data(QStringLiteral("image/png")) == QByteArray("\x89PNG fake"));
    CHECK(md && md->data(QStringLiteral("text/x-moz-text")) == QByteArray("hello world"));
    CHECK(p.writes == 1 && p.lastCharacters == 11);
    CHECK(p.lastTypes == (QStringList{QStringLiteral("text/plain"), QStringLiteral("image/png"),
                                      QStringLiteral("text/x-moz-text"), QStringLiteral("UTF8_STRING")}));

    // Data with no write before it: ignored, unanswered.
    p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("stray")));
    p.feed(k("type=wdata"));
    CHECK(p.take().isEmpty() && clip() == QLatin1String("hello world"));

    // Bad base64: EINVAL, then nothing until the next write.
    p.feed(k("type=write:id=w2"));
    p.feed(k("type=wdata:mime=" + b64("text/plain"), "!!bad!!"));
    p.feed(k("type=wdata"));
    CHECK(p.take() == k("type=write:status=EINVAL:id=w2"));
    CHECK(clip() == QLatin1String("hello world"));
    // Data without a type: EINVAL.
    p.feed(k("type=write"));
    p.feed(k("type=wdata", b64("x")));
    CHECK(p.take() == k("type=write:status=EINVAL"));
    // The id keeps only what's allowed.
    p.feed(k("type=write:id=a$b c/d-e_f+g.h"));
    p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("ok")));
    p.feed(k("type=wdata"));
    CHECK(p.take() == k("type=write:status=DONE:id=abcd-e_f+g.h"));
    CHECK(clip() == QLatin1String("ok"));

    // Reads: refused, but the list of types is given.
    p.feed(k("type=read:id=r1", b64("text/plain")));
    CHECK(p.take() == k("type=read:status=EPERM:id=r1"));
    p.feed(k("type=read:id=r2", b64(".")));
    const QByteArray list = p.take();
    CHECK(list.startsWith(k("type=read:status=OK:id=r2")));
    CHECK(list.contains("\033]5522;type=read:status=DATA:id=r2:mime=Lg==;"));
    CHECK(list.endsWith(k("type=read:status=DONE:id=r2")));
    {
        const int at = list.indexOf("mime=Lg==;") + 10;
        const QByteArray types = QByteArray::fromBase64(list.mid(at, list.indexOf('\033', at) - at));
        CHECK(types.contains("text/plain") && types.endsWith('\n'));
    }

    // The primary selection, where there's none: ENOSYS.
    if (!QGuiApplication::clipboard()->supportsSelection()) {
        p.feed(k("type=write:loc=primary:id=p"));
        CHECK(p.take() == k("type=write:status=ENOSYS:id=p"));
        p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("x")));
        p.feed(k("type=wdata"));
        CHECK(p.take().isEmpty());
        p.feed(k("type=read:loc=primary", b64(".")));
        CHECK(p.take() == k("type=read:status=ENOSYS"));
    }

    if (QGuiApplication::clipboard()->supportsSelection()) {
        p.feed(k("type=write:loc=primary:id=p"));
        p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("kitty primary")));
        p.feed(k("type=wdata"));
        p.settle();
        CHECK(p.take() == k("type=write:status=DONE:id=p"));
        CHECK(QGuiApplication::clipboard()->text(QClipboard::Selection) == QLatin1String("kitty primary"));
        CHECK(clip() == QLatin1String("ok"));
    }

    // Support detection: mode 5522, known and reset.
    p.feed("\033[?5522$p");
    CHECK(p.take() == "\033[?5522;2$y");

    // Off: nothing, unanswered.
    p.setClipboardWrites(false);
    p.feed(k("type=write"));
    p.feed(k("type=wdata:mime=" + b64("text/plain"), b64("off")));
    p.feed(k("type=wdata"));
    p.feed("\033[?5522$p");
    CHECK(p.take().isEmpty() && clip() == QLatin1String("ok"));
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    osc52();
    osc5522();
    if (failures == 0)
        QTextStream(stdout) << "test-clipboard: all passed (" << QGuiApplication::platformName() << ")\n";
    return failures ? 1 : 0;
}
