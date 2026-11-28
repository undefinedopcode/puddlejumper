/*
 * test-localpanes: a local tab's panes. A lone pane is a card when pane
 * cards are on (with the card's buttons, not the hover pill), and a split,
 * in either direction and inside another, halves the pane it splits.
 *
 * Runs offscreen; set PUDDLEJUMPER_SHELL to something harmless (the
 * default here is /bin/cat) and HOME/XDG_* to a scratch directory.
 */
#include "profiles.h"
#include "settings.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QSplitter>
#include <QTimer>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++failures;
}

void settle(int ms = 150)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Two widths (or heights) within a few pixels of each other.
bool halves(int a, int b)
{
    return std::abs(a - b) <= 8;
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("PUDDLEJUMPER_SHELL"))
        qputenv("PUDDLEJUMPER_SHELL", "/bin/cat");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("puddlejumper-test"));
    Settings settings;
    TerminalProfile profile;
    int nextId = 1;
    const auto makePane = [&]() { return new TerminalWidget(nextId++, settings, profile); };

    for (const bool cards : {false, true}) {
        settings.setPaneCards(cards);   // as the settings dialog does: panes read it
        TerminalWidget::setPaneCards(cards);
        const QString mode = cards ? QStringLiteral("cards: ") : QStringLiteral("no cards: ");

        auto *first = makePane();
        TerminalTab tab(first);
        tab.resize(1200, 800);
        tab.show();
        tab.updateFrames();
        settle();

        // (1) A lone pane.
        check(first->isCardShown() == cards, mode + QStringLiteral("a lone pane is a card only with cards on"));
        if (cards)
            check(tab.layoutRect() != tab.rect(), mode + QStringLiteral("a lone card has the gap round it"));
        // TEST_LOCALPANES_SHOT=PATH: a picture of the lone card.
        if (const QString shot = qEnvironmentVariable("TEST_LOCALPANES_SHOT"); cards && !shot.isEmpty())
            tab.grab().save(shot);

        // (2) Split top/bottom, then split the top one side by side.
        auto *second = makePane();
        tab.splitPane(first, Qt::Vertical, second);
        settle();
        check(halves(first->height(), second->height()),
              mode + QStringLiteral("top/bottom halves: %1 / %2").arg(first->height()).arg(second->height()));
        auto *third = makePane();
        tab.splitPane(first, Qt::Horizontal, third);
        settle();
        check(halves(first->width(), third->width()),
              mode + QStringLiteral("the top split side by side halves: %1 / %2").arg(first->width()).arg(third->width()));
        check(halves(first->height(), second->height()),
              mode + QStringLiteral("and the top/bottom split stays: %1 / %2").arg(first->height()).arg(second->height()));

        // And the other way round: side by side, then one of them top/bottom.
        auto *fourth = makePane();
        tab.splitPane(second, Qt::Horizontal, fourth);
        settle();
        auto *fifth = makePane();
        tab.splitPane(fourth, Qt::Vertical, fifth);
        settle();
        check(halves(fourth->height(), fifth->height()),
              mode + QStringLiteral("a side pane split top/bottom halves: %1 / %2").arg(fourth->height()).arg(fifth->height()));
        check(halves(second->width(), fourth->width()),
              mode + QStringLiteral("beside the one it was split from: %1 / %2").arg(second->width()).arg(fourth->width()));
        if (cards)
            check(first->isCardShown() && fifth->isCardShown(), mode + QStringLiteral("split panes are cards"));
    }

    std::printf(failures ? "%d failure(s)\n" : "test-localpanes: all passed\n", failures);
    return failures ? 1 : 0;
}
