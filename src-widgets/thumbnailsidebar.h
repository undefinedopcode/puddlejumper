/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "cardstyle.h"

#include <QColor>
#include <QWidget>

class QIcon;
class QListWidget;
class QMenu;
class QListWidgetItem;
class QPoint;
class QResizeEvent;
class QToolButton;

/*
 * Left-hand tab list: a card per tab with a live preview of its screen, its
 * title, a second line saying what it is (program, directory, tmux), and a
 * dot when it has had output you haven't seen.
 *
 * The sidebar only shows what it is told. The main window renders the
 * previews (at previewSize()) and owns the tabs; the sidebar reports what the
 * user did with the cards.
 */
class ThumbnailSidebar : public QWidget
{
    Q_OBJECT

public:
    explicit ThumbnailSidebar(QWidget *parent = nullptr);

    /* Replace the card list wholesale; call on tab add/remove/reorder. */
    void syncTabs(const QStringList &titles, int currentIndex);

    void setCurrentIndex(int index);
    void setTitle(int index, const QString &title);
    /* The card's second line: what's running, where, or which tmux session. */
    void setSubtitle(int index, const QString &subtitle);
    /* A small label on the preview, such as the tmux session's name. */
    void setBadge(int index, const QString &badge);
    void setPreview(int index, const QPixmap &pixmap);
    /* Mark a tab that produced output while it was not being watched. */
    void setBusy(int index, bool busy);
    /* Tint the cards whose program is waiting on the user. */
    void setTintBlocked(bool tint);
    /* What a program in the tab says it is doing (OSC 7501). */
    void setStatus(int index, const cardstyle::Status &status);
    /* Move one card, keeping its preview and marks (the tab bar moved a tab). */
    void moveCard(int from, int to);

    /*
     * The preview's shape, height over width: that of the terminal area, so a
     * card looks like the tab it stands for. Changes previewSize().
     */
    void setPreviewAspect(qreal aspect);
    /* The size previews are shown at, in logical pixels. */
    QSize previewSize() const;

    /*
     * Compact cards: a small thumbnail at the left with the title and
     * details beside it, a card about a third as tall (docs/
     * compact-sidebar.md). Changes previewSize() and the minimum width.
     */
    void setCompact(bool compact);
    bool isCompact() const { return m_compact; }

    /*
     * Colours from the terminal scheme, so the sidebar reads as part of the
     * terminal rather than window chrome: a slightly lifted background, cards
     * a little lighter again, text in the scheme's own colour.
     */
    void setColours(const QColor &background, const QColor &foreground);

    /*
     * The workspace header above the cards: its name and colour, opening
     * `menu` (the window's workspace menu) when clicked. Hidden while there
     * is only one workspace.
     */
    void setWorkspace(const QString &name, const QIcon &icon, QMenu *menu, bool show);
    QColor background() const { return m_background; }

signals:
    void tabActivated(int index);
    void tabClosed(int index);
    /* The user dragged a card from one place to another. */
    void tabMoved(int from, int to);
    void newTabRequested();
    void contextMenuRequested(int index, const QPoint &globalPos);
    /* previewSize() changed: previews need drawing again at the new size. */
    void previewSizeChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateItemSizes();
    QListWidgetItem *card(int index) const;
    void updateToolTip(QListWidgetItem *item);

    QListWidget *m_list = nullptr;
    QToolButton *m_newTab = nullptr;
    QToolButton *m_workspace = nullptr;
    qreal m_aspect = 0.6;
    bool m_compact = false;
    QSize m_lastPreviewSize;
    QColor m_background;
    QColor m_foreground;
    bool m_syncing = false;   // the list is being changed by us, not the user
};
