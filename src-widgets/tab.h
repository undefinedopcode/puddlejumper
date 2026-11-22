/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QWidget>

/*
 * A tab's page: what the window needs of any tab, whatever it shows, for its
 * card, its place in a workspace and its saved name. TerminalTab shows
 * terminals; VncTab a remote screen.
 */
class Tab : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

    /* Changes since the last preview, so its card is worth drawing again. */
    virtual bool hasNewOutput() const = 0;
    virtual void clearNewOutput() = 0;
    /* A picture of the tab exactly `size` (logical pixels), drawn even while
     * the tab is hidden. */
    virtual QPixmap previewPixmap(const QSize &size, qreal devicePixelRatio = 1.0) const = 0;

    /* The page's background and text colours, for the cards and strip. */
    virtual QColor backgroundColour() const = 0;
    virtual QColor foregroundColour() const = 0;

    /* Give the keyboard focus to what the tab shows. */
    virtual void focusContent() = 0;

    /* A name the user gave the tab, shown instead of its title; empty if none. */
    QString customTitle() const { return m_customTitle; }
    void setCustomTitle(const QString &title) { m_customTitle = title; }

private:
    QString m_customTitle;
};
