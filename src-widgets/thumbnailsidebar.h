/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

class QListWidget;
class QResizeEvent;

/*
 * Left-hand tab list with live terminal previews.
 *
 * The widgets counterpart of the QML thumbnail sidebar. Previews are real
 * grabs of each terminal's painted display; only the current tab can be
 * refreshed meaningfully, so other cards keep their last image.
 */
class ThumbnailSidebar : public QWidget
{
    Q_OBJECT

public:
    explicit ThumbnailSidebar(QWidget *parent = nullptr);

    void setPreviewSize(const QSize &size);

    /* Replace the card list wholesale; call on tab add/remove/reorder. */
    void syncTabs(const QStringList &titles, int currentIndex);

    void setPreview(int index, const QPixmap &pixmap);
    void setCurrentIndex(int index);
    void setTitle(int index, const QString &title);

    /* Mark a tab that produced output while it was not being watched. */
    void setBusy(int index, bool busy);

signals:
    void tabActivated(int index);
    void tabClosed(int index);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    /* Card size that fills the list viewport, so no dead strip remains. */
    QSize itemSize() const;

    QListWidget *m_list = nullptr;
    QSize m_previewSize{150, 90};
};
