/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QToolButton>

/*
 * A tool button drawn flat on every platform: nothing behind it at rest, a
 * rounded fill of the palette's Button colour while the mouse is over it, a
 * little stronger while pressed or its menu is open. The style draws only
 * its icon and text, and a drop-down (InstantPopup with a menu) gets the
 * app's own chevron. For the tab bars' buttons, which sit on the terminal's
 * colours outside a toolbar, where macOS's style draws QToolButtons as
 * native bevel buttons (a grey glass rectangle, even at rest).
 */
class FlatToolButton : public QToolButton
{
    Q_OBJECT

public:
    using QToolButton::QToolButton;

    /* Its own width, from what it draws (the style's would be for its
     * bevel); the style's height. */
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};
