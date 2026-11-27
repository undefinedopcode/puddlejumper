/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-tmuxsession: a tmux tab (TmuxSession) on a tmux server of its own
 * (tmux -L, removed at the end), with real panes on the offscreen platform.
 * Each pane's grid is checked against the size tmux gives its pane, split
 * and not, and the terminal margin is inside each pane (as for a local one):
 * a bigger margin, fewer cells, tmux told, the panes still in their tab.
 * PUDDLEJUMPER_TEST_PANE_CARDS=1 runs it with pane cards.
 *
 *   ./build/test-tmuxsession
 */

#include "qtermprobe.h"
#include "settings.h"
#include "terminaltab.h"
#include "terminalwidget.h"
#include "tmuxsession.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

#include "ScreenWindow.h"
#include "TerminalDisplay.h"

namespace {

int failures = 0;
int checks = 0;

bool check(bool ok, const QString &what)
{
    ++checks;
    QTextStream(ok ? stdout : stderr) << (ok ? "ok    " : "FAIL  ") << what << "\n";
    if (!ok)
        ++failures;
    return ok;
}

bool waitFor(const std::function<bool()> &done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QSize screenSize(TerminalWidget *pane)
{
    auto *d = pane ? pane->findChild<Konsole::TerminalDisplay *>() : nullptr;
    if (!d || !d->screenWindow())
        return {};
    return QSize(d->screenWindow()->screen()->getColumns(), d->screenWindow()->screen()->getLines());
}

QString socketName()
{
    return QStringLiteral("pj-test-%1").arg(QCoreApplication::applicationPid());
}

// tmux's own sizes for the session's panes, in pane order.
QList<QSize> tmuxPanes()
{
    QProcess p;
    p.start(QStringLiteral("tmux"), {QStringLiteral("-L"), socketName(), QStringLiteral("list-panes"), QStringLiteral("-t"),
                                     QStringLiteral("t"), QStringLiteral("-F"), QStringLiteral("#{pane_width}x#{pane_height}")});
    p.waitForFinished(5000);
    QList<QSize> out;
    for (const QString &line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList wh = line.split(QLatin1Char('x'));
        out << QSize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    return out;
}

// Every pane's grid is tmux's size for it (the same sizes in some order:
// the tab's pane order and tmux's needn't match).
bool agrees(TerminalTab *tab, QString *detail)
{
    QList<QSize> ours;
    for (TerminalWidget *p : tab->panes())
        ours << screenSize(p);
    QList<QSize> theirs = tmuxPanes();
    auto key = [](const QSize &a, const QSize &b) { return a.width() != b.width() ? a.width() < b.width() : a.height() < b.height(); };
    std::sort(ours.begin(), ours.end(), key);
    std::sort(theirs.begin(), theirs.end(), key);
    QStringList a, b;
    for (const QSize &s : ours)
        a << QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
    for (const QSize &s : theirs)
        b << QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
    *detail = QStringLiteral("ours %1, tmux %2").arg(a.join(' '), b.join(' '));
    return !ours.isEmpty() && ours == theirs;
}

// Every pane lies inside the tab (and `inset` pixels in from its edge).
bool inside(TerminalTab *tab, int inset = 0)
{
    for (TerminalWidget *p : tab->panes()) {
        const QRect r(p->mapTo(tab, QPoint(0, 0)), p->size());
        if (!tab->rect().adjusted(inset, inset, -inset, -inset).contains(r))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    if (QStandardPaths::findExecutable(QStringLiteral("tmux")).isEmpty()) {
        QTextStream(stdout) << "no tmux; skipped\n";
        return 0;
    }
    Settings settings;
    settings.setPaneCards(qEnvironmentVariableIntValue("PUDDLEJUMPER_TEST_PANE_CARDS") == 1);
    const TerminalProfile profile;

    // As the window does (applyAppearanceToAll), connected first as the
    // window's is (before any session's): every pane restyled.
    QList<QPointer<TerminalTab>> tabs;
    QObject::connect(&settings, &Settings::appearanceChanged, [&]() {
        for (auto &t : tabs)
            if (t)
                for (TerminalWidget *p : t->panes())
                    p->applyAppearance();
    });
    TmuxSession session(QStringLiteral("t"),
                        {QStringLiteral("tmux"), QStringLiteral("-L"), socketName(), QStringLiteral("-f"), QStringLiteral("/dev/null"),
                         QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("new-session"), QStringLiteral("-A"),
                         QStringLiteral("-s"), QStringLiteral("t"), QStringLiteral("/bin/sh")},
                        settings, profile);
    QObject::connect(&session, &TmuxSession::tabAdded, [&](TerminalTab *tab, const QString &) {
        tabs << tab;
        tab->resize(900, 520);
        tab->show();
        session.setClientArea(tab->layoutArea());   // as the session does on a resize
    });
    session.start();
    if (!check(waitFor([&] { return !tabs.isEmpty() && !tabs.first()->panes().isEmpty(); }), QStringLiteral("a tab")))
        return 1;
    TerminalTab *tab = tabs.first();
    QString detail;
    bool ok = waitFor([&] { return agrees(tab, &detail); });
    check(ok, QStringLiteral("one pane: tmux's size (%1)").arg(detail));

    session.splitPane(tab->activePane(), Qt::Horizontal);
    ok = waitFor([&] { return tab->panes().size() == 2 && agrees(tab, &detail); });
    check(ok, QStringLiteral("split: each pane tmux's size (%1)").arg(detail));
    check(inside(tab), QStringLiteral("split: the panes are inside the tab"));

    // The margin: inside each pane, so a bigger one leaves fewer cells.
    TerminalWidget *pane = tab->panes().value(0);
    auto *display = pane->findChild<Konsole::TerminalDisplay *>();
    const int margin = settings.appearance().terminalMargin;
    const QSize before = screenSize(pane);
    settings.setTerminalMargin(margin + 20);
    ok = waitFor([&] { return screenSize(pane).width() < before.width() && screenSize(pane).height() < before.height()
                              && agrees(tab, &detail); });
    check(ok, QStringLiteral("bigger margin: fewer cells, tmux told (%1; was %2x%3)")
                  .arg(detail).arg(before.width()).arg(before.height()));
    check(display && display->margin() == margin + 20,
          QStringLiteral("the margin is the pane's own: %1, want %2").arg(display ? display->margin() : -1).arg(margin + 20));
    check(inside(tab), QStringLiteral("bigger margin: the panes are inside the tab"));
    settings.setTerminalMargin(margin);
    ok = waitFor([&] { return screenSize(pane) == before && agrees(tab, &detail); });
    check(ok, QStringLiteral("margin back: the grid back (%1)").arg(detail));

    // The gap between panes, and round them: a bigger one, fewer cells too.
    // (MainWindow::applyAppearanceToAll() does this for every tab.)
    QObject::connect(&settings, &Settings::appearanceChanged, tab, [tab] { tab->applyPaneGap(); });
    const int gap = settings.appearance().paneGap;
    settings.setPaneGap(gap + 20);
    ok = waitFor([&] { return screenSize(pane).width() < before.width() && screenSize(pane).height() < before.height()
                              && agrees(tab, &detail); });
    check(ok, QStringLiteral("bigger gap: fewer cells, tmux told (%1; was %2x%3)")
                  .arg(detail).arg(before.width()).arg(before.height()));
    check(inside(tab, gap + 20), QStringLiteral("bigger gap: the panes are that far inside the tab"));
    settings.setPaneGap(gap);
    ok = waitFor([&] { return screenSize(pane) == before && agrees(tab, &detail); });
    check(ok, QStringLiteral("gap back: the grid back (%1)").arg(detail));

    session.killSession();
    waitFor([] { return false; }, 300);
    QProcess::execute(QStringLiteral("tmux"), {QStringLiteral("-L"), socketName(), QStringLiteral("kill-server")});
    QTextStream(stdout) << (failures ? "FAILED: " : "ok: ") << checks - failures << "/" << checks << " checks\n";
    return failures ? 1 : 0;
}
