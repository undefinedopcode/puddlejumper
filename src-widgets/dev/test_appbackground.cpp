/*
 * test-appbackground — the margin around the grid follows a full-screen
 * program's background (TerminalDisplay::setPaddingFollowsApp): on the
 * alternate screen, when most cells have one background of the program's,
 * and never on the shell's screen. Prints each failure; exits 1 if any.
 *
 * Run: ./build/test-appbackground (offscreen)
 */

#include "qtermwidget.h"
#include "TerminalDisplay.h"

#include <QApplication>
#include <QTextStream>
#include <QTimer>

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

const QColor kBlue(0x12, 0x34, 0x56);

void settle()
{
    // The emulator hands output to the display on a timer.
    QEventLoop loop;
    QTimer::singleShot(150, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Term
{
    explicit Term(QSize size = QSize(400, 300))
    {
        w.setMargin(12);
        w.resize(size);
        w.setScrollBarPosition(QTermWidget::NoScrollBar);
        w.startTerminalTeletype();
        w.show();
        d = w.findChild<Konsole::TerminalDisplay *>();
        d->setPaddingFollowsApp(true);
        settle();
    }
    void feed(const QByteArray &b)
    {
        w.feedData(b);
        settle();
    }
    // A pixel in the margin, left of the grid half way down.
    QColor margin()
    {
        return d->grab().toImage().pixelColor(4, d->height() / 2);
    }
    QTermWidget w;
    Konsole::TerminalDisplay *d = nullptr;
};

// An editor: its background over the whole screen (erase takes the current
// background), a statusline of another colour on the last row.
const QByteArray kEditor = "\033[?1049h\033[48;2;18;52;86m\033[2J\033[H"
                           "text\033[999;1H\033[48;2;200;0;0mstatus\033[K\033[48;2;18;52;86m";

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", qgetenv("QT_QPA_PLATFORM").isEmpty() ? "offscreen" : qgetenv("QT_QPA_PLATFORM"));
    QApplication app(argc, argv);

    {
        Term t;
        const QColor scheme = t.d->palette().window().color();
        CHECK(!t.d->appBackground().isValid());
        t.feed(kEditor);
        CHECK(t.d->appBackground() == kBlue);   // the statusline is the minority
        CHECK(t.margin() == kBlue);
        // Back to the shell: the scheme's background again.
        t.feed("\033[0m\033[?1049l");
        CHECK(!t.d->appBackground().isValid());
        CHECK(t.margin() == scheme);
        // The same colours on the shell's screen don't count.
        t.feed("\033[48;2;18;52;86m\033[2J");
        CHECK(!t.d->appBackground().isValid());
        t.feed("\033[0m\033[2J");
    }
    {
        // Mostly the scheme's background with an editor's theme in the
        // middle only: nothing to follow.
        Term t;
        t.feed("\033[?1049h\033[2J\033[5;5H\033[48;2;18;52;86mmiddle\033[0m");
        CHECK(!t.d->appBackground().isValid());
    }
    {
        // Short and wide, a tab bar on top and a statusline below: they're
        // most of the edge, but not of the window.
        Term t(QSize(900, 150));   // 6 lines
        t.feed(kEditor + "\033[H\033[48;2;200;0;0mtabs\033[K\033[48;2;18;52;86m");
        CHECK(t.d->appBackground() == kBlue);
    }
    {
        // Off: never.
        Term t;
        t.d->setPaddingFollowsApp(false);
        t.feed(kEditor);
        CHECK(!t.d->appBackground().isValid());
        // And on again, at once.
        t.d->setPaddingFollowsApp(true);
        CHECK(t.d->appBackground() == kBlue);
    }

    if (failures == 0)
        QTextStream(stdout) << "test-appbackground: all passed\n";
    return failures ? 1 : 0;
}
