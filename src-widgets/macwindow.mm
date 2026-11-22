/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "macwindow.h"

#include <QAction>
#include <QEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QToolBar>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace {

constexpr int kButtonGap = 10;   // between the window buttons and the toolbar

NSWindow *nsWindowOf(QWidget *window)
{
    return reinterpret_cast<NSView *>(window->winId()).window;
}

/*
 * Applies the title bar change once the native window exists (on first
 * show), and keeps the toolbar fitted to it afterwards.
 */
class TitleBarFilter : public QObject
{
public:
    TitleBarFilter(QMainWindow *window, QToolBar *toolbar)
        : QObject(window), m_window(window), m_toolbar(toolbar)
    {
        // The window buttons' room at the toolbar's left end.
        m_buttonSpace = new QWidget(toolbar);
        m_buttonSpaceAction = toolbar->insertWidget(toolbar->actions().value(0), m_buttonSpace);
        // Kept at the top: anywhere else the window buttons would be over
        // the terminal.
        toolbar->setMovable(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        // Ask Qt for the content under the title bar too, so it keeps it when
        // it sets the window's style again (leaving full screen does).
        window->setWindowFlags(window->windowFlags() | Qt::ExpandedClientAreaHint
                               | Qt::NoTitleBarBackgroundHint);
#endif
        window->installEventFilter(this);
        toolbar->installEventFilter(this);
        QObject::connect(toolbar, &QToolBar::visibilityChanged, this,
                         [this](bool visible) { fitContent(visible); });
    }

    ~TitleBarFilter() override
    {
        for (id observer in m_observers)
            [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [m_observers release];
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_window) {
            if (event->type() == QEvent::Show && !m_applied)
                apply();
            else if (event->type() == QEvent::WindowStateChange && m_applied) {
                fitButtonSpace();   // none in full screen
                restyle();
            }
        } else if (watched == m_toolbar && m_applied) {
            // Presses that reach the toolbar itself are on its empty space
            // (or the window buttons' room): a buttons' own presses stop at
            // the button.
            if (event->type() == QEvent::MouseButtonPress
                && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                if (QWindow *handle = m_window->windowHandle())
                    handle->startSystemMove();
                return true;
            }
            if (event->type() == QEvent::MouseButtonDblClick
                && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                [nsWindowOf(m_window) performZoom:nil];
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void apply()
    {
        NSWindow *win = nsWindowOf(m_window);
        if (!win)
            return;
        m_applied = true;
        styleWindow(win);

        // At least the title bar's height. The toolbar's own is usually a
        // little more, so the window buttons are moved down onto its centre.
        m_titleBar = qRound(win.frame.size.height - win.contentLayoutRect.size.height);
        m_toolbar->setFixedHeight(qMax(m_titleBar, m_toolbar->sizeHint().height()));
        fitButtonSpace();
        fitContent(m_toolbar->isVisible());
        hideTitleBarMaterial();
        centreWindowButtons();

        // AppKit lays the window buttons out again on these, and leaving
        // full screen also resets the window's style; put both back.
        NSNotificationName relayouts[] = {
            NSWindowDidResizeNotification, NSWindowDidExitFullScreenNotification,
            NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
            NSWindowDidChangeBackingPropertiesNotification,
        };
        m_observers = [[NSMutableArray alloc] init];
        for (NSNotificationName name : relayouts) {
            [m_observers addObject:[[NSNotificationCenter defaultCenter]
                                       addObserverForName:name
                                                   object:win
                                                    queue:nil
                                               usingBlock:^(NSNotification *) {
                                                   restyle();
                                               }]];
        }
    }

    // The title bar transparent and untitled, with the content under it.
    // Each is only set when it differs, as setting the style lays the
    // window out again.
    void styleWindow(NSWindow *win)
    {
        if (!(win.styleMask & NSWindowStyleMaskFullSizeContentView))
            win.styleMask |= NSWindowStyleMaskFullSizeContentView;
        if (!win.titlebarAppearsTransparent)
            win.titlebarAppearsTransparent = YES;
        if (win.titleVisibility != NSWindowTitleHidden)
            win.titleVisibility = NSWindowTitleHidden;
        if (win.titlebarSeparatorStyle != NSTitlebarSeparatorStyleNone)
            win.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    }

    // Everything again, outside full screen (which has its own title bar).
    void restyle()
    {
        NSWindow *win = nsWindowOf(m_window);
        if (!win || (m_window->windowState() & Qt::WindowFullScreen)
            || (win.styleMask & NSWindowStyleMaskFullScreen))
            return;
        styleWindow(win);
        fitButtonSpace();
        fitContent(m_toolbar->isVisible());
    }

    /*
     * Qt puts a title-bar material (an NSVisualEffectView) over the top of
     * its view when the content runs under the title bar. Over the toolbar it
     * would blur it and, being in the title bar, take its clicks to move the
     * window; the toolbar is the title bar's background here, so hide it.
     */
    void hideTitleBarMaterial()
    {
        NSView *view = reinterpret_cast<NSView *>(m_window->winId());
        for (NSView *child in view.subviews) {
            if ([child isKindOfClass:[NSVisualEffectView class]])
                child.hidden = m_toolbar->isVisible();
        }
    }

    // The window buttons on the toolbar's centre line rather than the
    // (shorter) title bar's, or back on the title bar's with the toolbar
    // hidden. Not in full screen, where AppKit shows them in its own title bar.
    void centreWindowButtons()
    {
        NSWindow *win = m_applied ? nsWindowOf(m_window) : nil;
        if (!win || (m_window->windowState() & Qt::WindowFullScreen))
            return;
        const qreal target = (m_toolbar->isVisible() ? m_toolbar->height() : m_titleBar) / 2.0;
        NSButton *zoom = [win standardWindowButton:NSWindowZoomButton];
        const NSRect frame = [zoom convertRect:zoom.bounds toView:nil];   // from the bottom
        const CGFloat centre = win.frame.size.height - NSMidY(frame);    // from the top
        const CGFloat delta = target - centre;                           // > 0: move down
        if (qAbs(delta) < 0.5)
            return;
        for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                    NSWindowZoomButton}) {
            NSButton *button = [win standardWindowButton:kind];
            NSPoint origin = button.frame.origin;
            origin.y += button.superview.isFlipped ? delta : -delta;
            [button setFrameOrigin:origin];
        }
    }

    // Room for the window buttons, except in full screen, where they hide.
    void fitButtonSpace()
    {
        NSWindow *win = m_applied ? nsWindowOf(m_window) : nil;
        if (!win)
            return;
        const bool fullScreen = m_window->windowState() & Qt::WindowFullScreen;
        NSButton *zoom = [win standardWindowButton:NSWindowZoomButton];
        const int right = qRound(NSMaxX([zoom convertRect:zoom.bounds toView:nil]));
        m_buttonSpace->setFixedWidth(right + kButtonGap - m_toolbar->contentsMargins().left());
        // A toolbar shows and hides its widgets through their actions.
        m_buttonSpaceAction->setVisible(!fullScreen);
    }

    // With the toolbar in the title bar the content starts at the window's
    // top edge. Without it, keep the content below the title bar, or the
    // window buttons would sit over the terminal.
    void fitContent(bool toolbarVisible)
    {
        if (!m_applied)
            return;
        // Qt's own safe-area margin, once turned on, doesn't go away when
        // turned off again, so it stays off and the margin is set here.
        m_window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
        m_window->setContentsMargins(0, toolbarVisible ? 0 : m_titleBar, 0, 0);
        hideTitleBarMaterial();
        centreWindowButtons();
    }

    QPointer<QMainWindow> m_window;
    QPointer<QToolBar> m_toolbar;
    QWidget *m_buttonSpace = nullptr;
    QAction *m_buttonSpaceAction = nullptr;
    NSMutableArray *m_observers = nil;
    int m_titleBar = 28;
    bool m_applied = false;
};

} // namespace

void unifyTitleBar(QMainWindow *window, QToolBar *toolbar)
{
    new TitleBarFilter(window, toolbar);
}
