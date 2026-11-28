/*
 * test-sidebargroups: the sidebar's cards grouped under headers
 * (ThumbnailSidebar::setGrouped/setGroup), with the tab order left as it is.
 */
#include "thumbnailsidebar.h"

#include <QAbstractItemModel>
#include <QColor>
#include <QApplication>
#include <QListWidget>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++failures;
}

constexpr int kTabRole = Qt::UserRole + 11;
constexpr int kHeaderRole = Qt::UserRole + 15;

// The rows as text: "[A]" for a header, the tab index for a card; hidden
// cards in brackets.
QString rows(QListWidget *list)
{
    QStringList out;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->data(kHeaderRole).toBool())
            out << QStringLiteral("[%1]").arg(item->data(Qt::UserRole + 13).toString());
        else
            out << (item->isHidden() ? QStringLiteral("(%1)") : QStringLiteral("%1")).arg(item->data(kTabRole).toInt());
    }
    return out.join(QLatin1Char(' '));
}

void settle()
{
    for (int i = 0; i < 3; ++i)
        QCoreApplication::processEvents();
}

void clickRow(QListWidget *list, int row)
{
    const QPoint at = list->visualItemRect(list->item(row)).center();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, at);
    settle();
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ThumbnailSidebar sidebar;
    sidebar.resize(260, 900);
    sidebar.show();
    auto *list = sidebar.findChild<QListWidget *>();

    const QStringList groups{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("A"), QStringLiteral("C"),
                             QStringLiteral("B")};
    sidebar.syncTabs({QStringLiteral("t0"), QStringLiteral("t1"), QStringLiteral("t2"), QStringLiteral("t3"),
                      QStringLiteral("t4")},
                     0);
    for (int i = 0; i < groups.size(); ++i)
        sidebar.setGroup(i, groups.at(i), groups.at(i), QString());
    settle();
    check(rows(list) == QStringLiteral("0 1 2 3 4"), QStringLiteral("not grouped: tab order, no headers: ") + rows(list));

    sidebar.setGrouped(true);
    settle();
    // TEST_SIDEBAR_SHOT=PREFIX: pictures of it grouped, normal and compact.
    if (const QString shot = qEnvironmentVariable("TEST_SIDEBAR_SHOT"); !shot.isEmpty()) {
        sidebar.setColours(QColor(0x1e, 0x1f, 0x29), QColor(0xe6, 0xe6, 0xe6));
        const char *icons[] = {"connections/terminal-tab-icon", "connections/tmux-tab-icon",
                               "connections/terminal-tab-icon", "connections/rex-tab-icon",
                               "connections/tmux-tab-icon"};
        const char *labels[] = {"Local", "main", "Local", "atlantis", "main"};
        for (int i = 0; i < 5; ++i) {
            sidebar.setGroup(i, groups.at(i), QString::fromLatin1(labels[i]), QString::fromLatin1(icons[i]));
            sidebar.setIcon(i, QString::fromLatin1(icons[i]));
            sidebar.setSubtitle(i, QStringLiteral("~/projects"));
        }
        clickRow(list, 3);   // collapse B
        cardstyle::Status waiting;
        waiting.kind = cardstyle::Status::Blocked;
        waiting.label = QStringLiteral("Needs approval");
        sidebar.setStatus(1, waiting);
        sidebar.setBusy(4, true);
        settle();
        sidebar.grab().save(shot + QStringLiteral("-cards.png"));
        sidebar.setCompact(true);
        settle();
        sidebar.grab().save(shot + QStringLiteral("-compact.png"));
        sidebar.setCompact(false);
        clickRow(list, 3);
        sidebar.setStatus(1, cardstyle::Status());
        sidebar.setBusy(4, false);
        for (int i = 0; i < 5; ++i)
            sidebar.setGroup(i, groups.at(i), groups.at(i), QString());
        settle();
    }
    check(rows(list) == QStringLiteral("[A] 0 2 [B] 1 4 [C] 3"), QStringLiteral("grouped: ") + rows(list));

    // Clicking a card picks its tab, not its row.
    QSignalSpy activated(&sidebar, &ThumbnailSidebar::tabActivated);
    clickRow(list, 5);   // tab 4
    check(!activated.isEmpty() && activated.last().at(0).toInt() == 4, QStringLiteral("a card activates its tab"));

    // A header collapses its group, and opens it again.
    clickRow(list, 3);   // [B]
    check(rows(list) == QStringLiteral("[A] 0 2 [B] (1) (4) [C] 3"), QStringLiteral("collapsed: ") + rows(list));
    check(activated.size() == 1, QStringLiteral("a header activates nothing"));
    // Shut, its header counts the programs waiting on the user, and
    // whether any card has unseen output.
    {
        constexpr int kBlockedCountRole = Qt::UserRole + 18;
        constexpr int kAnyBusyRole = Qt::UserRole + 19;
        cardstyle::Status waiting;
        waiting.kind = cardstyle::Status::Blocked;
        sidebar.setStatus(1, waiting);
        sidebar.setStatus(4, waiting);
        settle();
        check(list->item(3)->data(kBlockedCountRole).toInt() == 2, QStringLiteral("the header counts 2 waiting"));
        sidebar.setStatus(4, cardstyle::Status());
        settle();
        check(list->item(3)->data(kBlockedCountRole).toInt() == 1, QStringLiteral("and 1 when one is answered"));
        check(list->item(0)->data(kBlockedCountRole).toInt() == 0, QStringLiteral("another group's header counts none"));
        check(!list->item(3)->data(kAnyBusyRole).toBool(), QStringLiteral("no unseen output yet"));
        sidebar.setBusy(4, true);
        check(list->item(3)->data(kAnyBusyRole).toBool(), QStringLiteral("unseen output in the group"));
        sidebar.setBusy(4, false);
        sidebar.setStatus(1, cardstyle::Status());
    }
    sidebar.setCurrentIndex(1);
    settle();
    check(rows(list) == QStringLiteral("[A] 0 2 [B] 1 4 [C] 3"),
          QStringLiteral("a tab chosen elsewhere opens its group: ") + rows(list));

    // A tab moved in the tab bar: groups follow their first tab.
    sidebar.moveCard(1, 0);   // tabs: t1 t0 t2 t3 t4 → B A A C B
    settle();
    check(rows(list) == QStringLiteral("[B] 0 4 [A] 1 2 [C] 3"), QStringLiteral("after a move: ") + rows(list));

    // A card changing group (a tab attached elsewhere, say).
    sidebar.setGroup(3, QStringLiteral("A"), QStringLiteral("A"), QString());
    settle();
    check(rows(list) == QStringLiteral("[B] 0 4 [A] 1 2 3"), QStringLiteral("regrouped: ") + rows(list));

    // Dragging within a group moves the tab next to the card it lands by.
    QSignalSpy moved(&sidebar, &ThumbnailSidebar::tabMoved);
    // Rows: [B]0 0 4 [A]3 1 2 3 — move tab 3 (row 6) before tab 1 (row 4).
    list->model()->moveRow(QModelIndex(), 6, QModelIndex(), 4);
    settle();
    check(moved.size() == 1 && moved.last().at(0).toInt() == 3 && moved.last().at(1).toInt() == 1,
          QStringLiteral("dragged within its group: tabMoved(3, 1)"));
    check(rows(list) == QStringLiteral("[B] 0 4 [A] 1 2 3"), QStringLiteral("renumbered: ") + rows(list));
    check(list->item(4)->data(Qt::UserRole + 1).toString() == QStringLiteral("t3"),
          QStringLiteral("the dragged card is now tab 1"));

    // Dropped into another group: no move, and it goes back.
    moved.clear();
    list->model()->moveRow(QModelIndex(), 4, QModelIndex(), 1);   // an A card to the top of B
    settle();
    check(moved.isEmpty(), QStringLiteral("dropped in another group: no move"));
    check(rows(list) == QStringLiteral("[B] 0 4 [A] 1 2 3"), QStringLiteral("and back in its group: ") + rows(list));

    // One group only: no headers.
    for (int i = 0; i < 5; ++i)
        sidebar.setGroup(i, QStringLiteral("A"), QStringLiteral("A"), QString());
    settle();
    check(rows(list) == QStringLiteral("0 1 2 3 4"), QStringLiteral("one group, no headers: ") + rows(list));

    // Tabs closing: cards go, headers of empty groups too.
    sidebar.setGroup(4, QStringLiteral("Z"), QStringLiteral("Z"), QString());
    settle();
    sidebar.syncTabs({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")}, 0);
    settle();
    check(rows(list) == QStringLiteral("0 1 2 3"), QStringLiteral("a group's last tab closed: ") + rows(list));

    std::printf(failures ? "%d failure(s)\n" : "test-sidebargroups: all passed\n", failures);
    return failures ? 1 : 0;
}
