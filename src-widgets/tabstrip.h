/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "cardstyle.h"

#include <QColor>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QMouseEvent;
class QPoint;
class QToolButton;

/*
 * The tabs along the top: a row of cards drawn like the thumbnail sidebar's
 * (cardstyle.h), without the previews. Used in place of QTabBar, whose look
 * varies by platform and is poor on macOS.
 *
 * Like the sidebar it only shows what it is told; the main window owns the
 * tabs and keeps it in step, and it reports what the user did.
 */
class TabStrip : public QWidget
{
    Q_OBJECT

public:
    explicit TabStrip(QWidget *parent = nullptr);

    /* Replace the tab list wholesale; call on tab add/remove/reorder. */
    void syncTabs(const QStringList &titles, int currentIndex);

    void setCurrentIndex(int index);
    void setTitle(int index, const QString &title);
    void setTabToolTip(int index, const QString &toolTip);
    /* Mark a tab that produced output while it was not being watched. */
    void setBusy(int index, bool busy);
    /* What a program in the tab says it is doing (OSC 7501): a dot in its
     * colour. */
    void setStatus(int index, const cardstyle::Status &status);
    /* Move one tab, keeping its marks (the tab moved elsewhere). */
    void moveTab(int from, int to);

    /* Colours from the terminal scheme; see ThumbnailSidebar::setColours(). */
    void setColours(const QColor &background, const QColor &foreground);

signals:
    void tabActivated(int index);
    void tabClosed(int index);
    /* The user dragged a tab from one place to another. */
    void tabMoved(int from, int to);
    void newTabRequested();
    void contextMenuRequested(int index, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QListWidgetItem *item(int index) const;
    void updateItemWidth(QListWidgetItem *item);
    /*
     * Fit the tabs to the room there is: as wide as their titles want if
     * they all fit, otherwise the widest narrowed first, down to the minimum,
     * and past that the row scrolls.
     */
    void layoutTabs();
    /* Show the list of all tabs only while some are scrolled out of sight. */
    void updateOverflow();
    void showAllTabsMenu();

    QListWidget *m_list = nullptr;
    QToolButton *m_allTabs = nullptr;
    QToolButton *m_newTab = nullptr;
    QColor m_background;
    QColor m_foreground;
    bool m_syncing = false;   // the list is being changed by us, not the user
};
