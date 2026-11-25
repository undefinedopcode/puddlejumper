/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "tab.h"
#include "terminalwidget.h"

#include <QJsonObject>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QPainter;
class QSplitter;
class QVBoxLayout;

/*
 * A layout of panes in cells, as tmux describes a window: a leaf is a pane,
 * a branch arranges its children side by side (Qt::Horizontal) or stacked
 * (Qt::Vertical). Sizes are in character cells; siblings are separated by one
 * cell, which is where the splitter handle goes.
 */
struct PaneLayout
{
    int paneId = -1;                                // leaves only
    int columns = 0;
    int lines = 0;
    Qt::Orientation orientation = Qt::Horizontal;   // branches only
    QVector<PaneLayout> children;

    bool isLeaf() const { return children.isEmpty(); }
};

/*
 * A layer floating over a tab's tiled panes: a frame (a title strip to drag
 * it by, a border to resize it by) holding a layout of its own. `position` is
 * the frame's top left within the layout area, in pixels; its size follows
 * from the layout's cells plus TerminalTab::floatingChrome().
 */
struct FloatingLayout
{
    QString id;
    QPoint position;
    PaneLayout layout;
};

/*
 * One tab: a tree of terminal panes in nested splitters.
 *
 * A tab starts with one pane and splits from there. The active pane is the
 * one with keyboard focus (or the last one that had it); it is what the
 * window's actions act on, and the only one framed in the highlight colour.
 *
 * tmux tabs are laid out by tmux instead, through applyLayout(), which sizes
 * every pane and handle to whole character cells so each pane shows exactly
 * the grid tmux gave it.
 */
class TerminalTab : public Tab
{
    Q_OBJECT

public:
    explicit TerminalTab(TerminalWidget *firstPane, QWidget *parent = nullptr);

    TerminalWidget *activePane() const { return m_active; }
    /* In layout order: left to right, top to bottom. */
    QList<TerminalWidget *> panes() const;
    bool contains(const TerminalWidget *pane) const;

    /* Put pane beside `at` (to the right, or below), and make it active. */
    void splitPane(TerminalWidget *at, Qt::Orientation orientation, TerminalWidget *pane);
    /* Take a pane out and delete it. empty() fires when it was the last. */
    void removePane(TerminalWidget *pane);

    void setActivePane(TerminalWidget *pane, bool giveFocus = true);
    /* Move the focus to the next (+1) or previous (-1) pane. */
    void cyclePane(int step);

    /*
     * Lay the tab out as `layout` says. Leaves are resolved with paneFor(),
     * which returns an existing pane or makes a new one; panes the layout no
     * longer mentions are deleted. Each pane is sized to cell * its cells plus
     * its chrome, and each handle to one cell minus the chrome, so the pixels
     * add up exactly. Panes also get their screen size pinned to the layout's.
     */
    void applyLayout(const PaneLayout &layout,
                     const std::function<TerminalWidget *(int paneId)> &paneFor,
                     const QSize &cell, const QSize &chrome,
                     const QList<FloatingLayout> &floating = {},
                     const QList<int> &hidden = {});

    /* A floating frame's pixels round its layout: border and title strip. */
    static QSize floatingChrome();
    /* The floating layer a pane is in; empty when it is tiled. */
    QString floatingLayerOf(const TerminalWidget *pane) const;
    /* The layout area within the tab (floating frames are kept inside it). */
    QRect layoutRect() const;

    /*
     * Space round the whole layout, filled with the terminal background. tmux
     * tabs use it in place of each pane's own margin (see TerminalWidget's
     * applyAppearance()).
     */
    void setPadding(int pixels);
    /* The size a layout has to fit in: the tab less its padding. */
    QSize layoutArea() const;

    /* Frame panes even when there is only one, so a pane's chrome is the same
     * whether or not its window is split. tmux tabs need that for exact sizing. */
    void setAlwaysFramed(bool on);

    /*
     * The arrangement, for saving: splits ("split": "right" or "down", with
     * "sizes" in proportion and "panes" in order) down to the panes, each
     * described by describe(). "active" is the active pane's index in
     * panes() order.
     */
    QJsonObject saveLayout(const std::function<QJsonObject(TerminalWidget *)> &describe) const;
    /*
     * The reverse: a tab laid out as saveLayout() said, each pane made by
     * makePane() from its description. Null when the layout names no pane
     * that could be made.
     */
    static TerminalTab *restoreLayout(const QJsonObject &layout,
                                      const std::function<TerminalWidget *(const QJsonObject &)> &makePane);

    bool hasNewOutput() const override;
    void clearNewOutput() override;
    /*
     * A picture of the tab exactly `size` (logical pixels): each pane's screen
     * in its share of the box, drawn from the panes' screens so it works while
     * the tab is hidden.
     */
    QPixmap previewPixmap(const QSize &size, qreal devicePixelRatio = 1.0) const override;

    /* The terminal background, for padding and splitter handles. */
    QColor backgroundColour() const override;
    /* The terminal's default text colour. */
    QColor foregroundColour() const override;

    /* To the active pane. */
    void focusContent() override;

signals:
    void activePaneChanged(TerminalWidget *pane);
    void paneAdded(TerminalWidget *pane);
    void empty();
    /* The panes' area changed size, or the tab was shown: a hidden one is
     * resized unseen, and shown at that size without another resize. */
    void layoutAreaResized();
    /*
     * The user dragged a splitter handle. `before` is a pane in the part
     * before the handle, and `pixels` that part's new extent along the
     * splitter. tmux tabs turn this into resize-pane; local tabs ignore it.
     */
    void splitterDragged(TerminalWidget *before, Qt::Orientation orientation, int pixels);
    /* A floating frame moved or resized by the user, to `frame` (relative to
     * the layout area); it stays there until the next applyLayout(). */
    void floatingGeometryChanged(const QString &layerId, const QRect &frame);
    void floatingCloseRequested(const QString &layerId);
    /* One of a floating layer's panes became active while the layer was
     * under another. */
    void floatingRaiseRequested(const QString &layerId);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QSplitter *makeSplitter(Qt::Orientation orientation);
    /* Put `to` where `from` is in the tree (a splitter slot or the root). */
    void replaceInTree(QWidget *from, QWidget *to);
    /* Drop a splitter left with one child, putting the child in its place. */
    void collapse(QSplitter *splitter);
    void collectPanes(QWidget *node, QList<TerminalWidget *> &out) const;
    TerminalWidget *firstPaneIn(QWidget *node) const;
    TerminalWidget *paneContaining(QWidget *widget) const;
    void updateFrames();
    void paintPreview(QPainter &painter, QWidget *node, const QRect &box,
                      qreal devicePixelRatio) const;
    void watchPane(TerminalWidget *pane);

    QJsonObject saveNode(QWidget *node,
                         const std::function<QJsonObject(TerminalWidget *)> &describe) const;
    QWidget *restoreNode(const QJsonObject &node,
                         const std::function<TerminalWidget *(const QJsonObject &)> &makePane);

    QWidget *buildLayout(const PaneLayout &node,
                         const std::function<TerminalWidget *(int paneId)> &paneFor,
                         QList<TerminalWidget *> &used);
    void sizeLayout(QWidget *node, const PaneLayout &layout,
                    const QSize &cell, const QSize &chrome);

    QVBoxLayout *m_layout = nullptr;
    QWidget *m_root = nullptr;      // a TerminalWidget or a QSplitter
    QPointer<TerminalWidget> m_active;
    bool m_alwaysFramed = false;
    int m_padding = 0;
    QString m_layoutShape;          // applyLayout(): structure of the last layout
    QString m_floatingShape;        // and of its floating layers
    struct Floating
    {
        QString id;
        QPointer<QWidget> frame;    // a FloatingFrame
        QPointer<QWidget> root;     // its pane or splitter
    };
    QList<Floating> m_floating;     // bottom to top
    // applyLayout()'s hidden panes: kept (output still arrives) but not laid
    // out, nor among panes(); a zoomed window's others.
    QList<QPointer<TerminalWidget>> m_parked;
    bool m_applyingLayout = false;
};
