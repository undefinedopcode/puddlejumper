/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "profiles.h"
#include "rexkeymap.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QUrl>

#include <functional>
#include <map>
#include <memory>

class QTimer;
class RexControl;
class RexFileCopy;
class RexTerminalConnection;
class Settings;

namespace GhosttySnapshot {
struct Snapshot;
}

/*
 * A session on a Rex server, shown as tmux sessions are: each of its windows
 * a tab, each terminal block in a window's tiled layout a pane. The server's
 * view of the session is what the tabs show, and changes to it (from here or
 * from any other client) are followed; new tab, split, close and rename here
 * ask the server, and the view change that follows does the rest.
 *
 * Each block has its own data stream: a snapshot, then live output, into an
 * External pane. What a pane sends (keys, pastes, mouse reports, encoded by
 * the emulator as for a pty) is written to its block, and a pane's size, in
 * cells, becomes its block's size. Rex layouts are proportions, so the cells
 * are worked out here from the tab's area, and each block is told its own.
 *
 * When a block's program exits the server closes the block (and the view
 * changes), unless it failed at once: then the block is kept so its output
 * can be read, and closing the pane closes it. A session left with nothing
 * in it is ended rather than left behind empty.
 *
 * A window's floating layers float over its tiled panes, each a frame placed
 * by its bounds (proportions of the window) and stacked in the server's order,
 * holding a layout of its own. Moving or resizing a frame, closing it, or
 * using a pane in it (which raises it) asks the server; panes can be floated
 * out of the tiled layout and docked back into it.
 *
 * Detaching leaves the session running, to be attached again from any Rex
 * client.
 */
class RexSession : public QObject
{
    Q_OBJECT

public:
    /* server: http://host:port. sessionId: one to attach, or empty for a new
     * session running the server's default shell. */
    RexSession(const QUrl &server, const QString &sessionId, const Settings &settings,
               const TerminalProfile &profile, QObject *parent = nullptr);
    ~RexSession() override;

    void start();
    /* Leave the session running; ended() follows. */
    void detach();
    /* End the session on the server; ended() follows. */
    void destroySession();

    QUrl server() const { return m_server; }
    QString sessionId() const { return m_sessionId; }
    QString label() const { return m_label; }
    bool isAttached() const { return m_attached; }

    /*
     * A lost connection doesn't end the session here: its tabs stay, under
     * an overlay, while it reconnects (kRetryDelays, then it waits for
     * Reconnect), and attaches again when the server is back. A session
     * that is attached, or waiting like that, is saved with the window.
     */
    bool isOffline() const { return m_offline; }
    bool shouldRestore() const { return m_attached || m_offline; }
    /* Try now (the overlay's Reconnect). */
    void reconnectNow();

    /* Its windows as last seen, saved so that a restore while the server
     * is away can show them (as placeholders) until it is back. */
    struct SavedWindow
    {
        QString id;
        QString label;
    };
    QList<SavedWindow> savedWindows() const;
    /* Before start(): this is a restore; if the server can't be reached,
     * these windows (or one, named `label`) wait for it. */
    void setRestoring(const QList<SavedWindow> &windows, const QString &label);

    QList<TerminalTab *> tabs() const;
    bool ownsTab(const Tab *tab) const;
    bool ownsPane(const TerminalWidget *pane) const;
    QString windowName(const TerminalTab *tab) const;
    QString blockId(const TerminalWidget *pane) const;
    TerminalWidget *paneForBlock(const QString &blockId) const;
    /* The tab showing a window of this session, or (windowId empty) its
     * active window, else its first; null if it shows none. */
    TerminalTab *tabForWindow(const QString &windowId) const;
    /* Whether another client sizes this pane's block (until a key or a
     * click here takes it). */
    bool isFollowing(const TerminalWidget *pane) const;
    /* Whether this pane's program has exited (its block kept to be read). */
    bool hasExited(const TerminalWidget *pane) const;

    void newWindow();
    void splitPane(TerminalWidget *pane, Qt::Orientation orientation);
    /* Closes are undoable: undoClose() reopens this client's most recent
     * one (pane or window), as it was, for about a minute after; `done`
     * gets an empty string, or why not (nothing to undo). */
    void closePane(TerminalWidget *pane);
    void closeWindow(TerminalTab *tab);
    void undoClose(std::function<void(const QString &error)> done = {});
    void renameWindow(TerminalTab *tab, const QString &name);
    /* Rename the session itself (its label). */
    void renameSession(const QString &name);
    /* A window's tab was brought to the front here: the server's active
     * window follows, so other clients see the same one. */
    void windowShown(TerminalTab *tab);
    /* Whether a pane is in a floating layer. */
    bool isFloating(const TerminalWidget *pane) const;
    /* Float a tiled pane over the window where it sits, or dock a floating
     * one back into the tiled layout where it is: beside the tiled pane
     * under (or nearest) its middle, on the side it leans to. Floating a
     * pane and docking it straight back puts it where it was; moving the
     * float first moves the pane. */
    void floatPane(TerminalWidget *pane);
    void dockPane(TerminalWidget *pane);
    /* Swap a pane with the next in its window (wrapping round). */
    void swapWithNext(TerminalWidget *pane);
    /* Move a pane into another of the session's windows, beside its focused
     * tiled pane. A window left empty goes. */
    void movePane(TerminalWidget *pane, TerminalTab *to);
    /* Zoom a pane to fill its window (the others hidden), or back. */
    void toggleZoom(TerminalWidget *pane);
    bool isZoomed(const TerminalTab *tab) const;
    /* Whether this pane is shown zoomed (either of a window's two zooms). */
    bool isZoomed(const TerminalWidget *pane) const;

    /* Keymaps and actions. Before start(): what client.keymap is sent (this
     * app's actions and default bindings); without it no keymap is fetched.
     * The keymap is fetched once attached and again when the server's
     * configuration changes (keymapChanged). */
    void setKeymapRequest(std::function<QJsonObject()> request) { m_keymapRequest = std::move(request); }
    const RexKeymap &keymap() const { return m_keymap; }
    /* Run one of the server's actions (client.action) on a pane's block
     * (or the session's focused one); actionFinished or actionFailed
     * follows. origin: "key", "palette", "menu" or "api". */
    void runAction(const QString &name, const QJsonObject &args, const TerminalWidget *pane, const QString &origin);
    /* Give the server this client's own Lua (client.set_script: [{name,
     * source}], empty to remove them): key handlers, bound to
     * client.key.forward in the keymap, which is fetched again. `done` gets
     * the reply ({bindings, diagnostics}) or an error. */
    void setScripts(const QJsonArray &scripts,
                    std::function<void(const QJsonObject &reply, const QJsonObject &error)> done = {});
    /* Send the scripts file in the settings (rexScriptsFile), if any, or
     * remove scripts sent before; done on attaching, and when the setting
     * changes. A file that can't be read, or Lua the server refuses, is
     * actionFailed(<file name>, …); a server without client scripts is only
     * logged. `done`, if given, hears how it went: ok, or why not (that
     * server included). Loaded, the message is the server's diagnostics,
     * if any (what it skipped or ignored). */
    void sendScripts(std::function<void(bool ok, const QString &message)> done = {});
    /* Send a key to the server (client.key) for a pane's block: one bound
     * to client.key.forward, after the keys of its sequence before it
     * (prefix). The server's handler for it runs, or the server types it;
     * a failure is actionFailed("client.key.forward", …). */
    void forwardKey(const TerminalWidget *pane, const QJsonObject &key, const QStringList &prefix);
    /* Answer a server's call (performRequested, promptRequested): a result,
     * or an error {code, message}. */
    void replyCall(const QString &callId, const QJsonValue &result, const QJsonObject &error = {});
    /* This connection's client id, as the server knows it. */
    QString clientId() const;

    /* Copying files (the server's fs block, API revision 23). Whether the
     * server is this machine's own, over its Unix socket: then its files are
     * these, and a file dropped on a pane is typed as its path as usual;
     * otherwise a drop is filesDropped(). */
    bool isLocal() const;
    /* The directory a pane's program last reported (OSC 7), as a path on
     * the server; empty if it hasn't said. */
    void paneDirectory(const TerminalWidget *pane, std::function<void(const QString &dir)> done);
    /* A copy to or from the server in this session: start it with upload()
     * or download(). The session owns it (it goes if the session does);
     * delete it when finished. */
    RexFileCopy *newFileCopy();

    /* The sessions on a server, for a menu: calls back with (id, label)
     * pairs, or an error. */
    static void listSessions(const QUrl &server, QObject *context,
                             std::function<void(const QList<QPair<QString, QString>> &, const QString &error)> done);

signals:
    /* A window appeared; the receiver shows `tab`. */
    void tabAdded(TerminalTab *tab, const QString &name);
    void tabRenamed(TerminalTab *tab, const QString &name);
    /* The session's label changed (here or elsewhere). */
    void labelChanged(const QString &label);
    /* A window went; the receiver takes the tab away and deletes it. */
    void tabRemoved(TerminalTab *tab);
    /* Gone: detached, destroyed, emptied, or the connection lost for good.
     * All tabs have been removed already. `reason` may be empty. */
    void ended(const QString &reason);
    /* A (new) keymap arrived. */
    void keymapChanged();
    /* The server (for another client, or an action of its own) asks this
     * client to run one of its actions; answer with replyCall(). `pane` is
     * the block it names, if it is one of this session's. */
    void performRequested(const QString &callId, const QString &action, const QJsonObject &args,
                          TerminalWidget *pane);
    /* An action asks a question (client_prompt_requested): {kind: input |
     * pick, title, placeholder, value, items, multiple, allow_other}; answer
     * {text}, {ids} or {cancelled: true} with replyCall(). */
    void promptRequested(const QString &callId, const QJsonObject &prompt);
    /* The question is no longer wanted; close it without answering. */
    void promptEnded(const QString &callId);
    /* An action this client ran (runAction) queued one of the client's own
     * actions (rex.client.queue): perform it, as Rex leaves that to the
     * caller. Each comes in order, before actionFinished. */
    void queuedAction(const QString &fromAction, const QString &action, const QJsonObject &args,
                      TerminalWidget *pane);
    void actionFinished(const QString &action, const QJsonValue &result);
    /* Local files were dropped on one of the panes (a server elsewhere). */
    void filesDropped(TerminalWidget *pane, const QStringList &paths);
    void actionFailed(const QString &action, const QString &message);

private:
    struct Block
    {
        QString id;
        int paneId = -1;
        QPointer<TerminalWidget> pane;
        RexTerminalConnection *stream = nullptr;
        QSize screen;  // the block's size, as the server last said
        QSize sent;    // the size last asked for
        QSize sentCell;  // the cell size, in pixels, sent with it
        int resizing = 0;  // resize requests not yet answered
        int retries = 0;   // asked again, the server having kept another size
        bool exited = false;
        int exitCode = 0;
        QString label;  // the server's name for it, if any
        QString owner;  // the client whose size it follows (empty: none)
        bool taking = false;  // session.own_block (take) on its way
    };
    struct Window
    {
        QString id;
        QString label;
        QPointer<TerminalTab> tab;
        QJsonObject layout;  // the tiled layer's layout tree
        struct Layer
        {
            QString id;
            QRectF bounds;       // proportions of the window
            QJsonObject layout;
        };
        QList<Layer> floating;   // bottom to top
        QString focusedBlock;
        // Zooms (API revision 21): zoomedBlock is the focused block when it
        // is zoomed; tiledZoom the tiled layer's zoomed block, drawn over the
        // window behind the floating layers. A zoomedBlock that isn't the
        // tiled zoom (a floating block's, or any from an older server) is
        // drawn over everything.
        QString zoomedBlock;
        QString tiledZoom;
    };

    void attach();
    void fetchView();
    void applyView(const QJsonObject &view);
    void layoutWindow(Window &w);
    Block *block(const QString &id);
    TerminalWidget *makePane(Block *b);
    void openStream(Block *b);
    void dropBlock(const QString &id);
    void onSnapshot(Block *b, const GhosttySnapshot::Snapshot &snapshot);
    void onMarker(Block *b, const QJsonObject &marker);
    void onEvent(const QJsonObject &event);
    void fetchKeymap();
    void sendInput(Block *b, const QByteArray &bytes);
    void sendSize(Block *b, const QSize &cells, bool again = false);
    void checkSize(Block *b);
    void sendTheme(Block *b);
    /* Block owners. Another client's block keeps its size and the pane
     * says so (following); typing or clicking in the pane takes it. */
    bool following(const Block *b) const;
    void setOwner(Block *b, const QString &owner);
    void showFollowing(Block *b);
    // A program's clipboard write (clipboard_written), put on this
    // computer's clipboard; read in chunks when the event didn't carry it.
    void takeClipboardWrite(const QString &blockId, const QJsonObject &event);
    void readClipboardWrite(const QString &blockId, qint64 id, qint64 offset, const QString &sofar);
    void applyClipboardWrite(const QString &blockId, const QString &text);
    void takeBlock(Block *b);
    void requestBlocks();
    void splitterDragged(Window &w, TerminalWidget *before, Qt::Orientation orientation, int pixels);
    void floatingMoved(Window &w, const QString &layerId, const QRect &frame);
    /* A pane's rectangle in its tab's layout area, as proportions (empty if
     * it isn't shown). */
    static QRectF paneBounds(const TerminalTab *tab, const TerminalWidget *pane);
    /* A window's layout area, and its size in cells. */
    QSize windowArea(const Window &w);
    /* Where a floating layer's frame goes in `area`, and its layout's cells. */
    QRect floatingFrame(const QRectF &bounds, const QSize &area, QSize *cells);
    void finish(const QString &reason);
    void endIfEmpty();
    /* The connection is gone: keep the tabs, and try again. */
    void goOffline(const QString &reason);
    void scheduleReconnect();
    void tryReconnect();
    /* The overlays' text, on every window's tab (none: removed). */
    void showOffline();
    void hideOffline();
    Window *windowOf(const TerminalTab *tab);
    const Window *windowOf(const TerminalTab *tab) const;
    QJsonObject target(const Block *b) const;
    QSize cellSize();
    QSize paneChrome() const;

    QUrl m_server;
    QString m_sessionId;
    QString m_label;
    QString m_activeWindow;  // the server's active window
    const Settings &m_settings;
    TerminalProfile m_profile;
    RexControl *m_control = nullptr;
    QTimer *m_viewTimer = nullptr;
    bool m_viewPending = false;  // a view request in flight
    bool m_viewAgain = false;    // and another change since
    qint64 m_revision = -1;
    QList<Window> m_windows;
    std::map<QString, std::unique_ptr<Block>> m_blocks;
    QHash<int, QString> m_paneBlocks;  // pane id -> block id
    QPointer<TerminalWidget> m_metrics;  // hidden; measures cells
    QSize m_area;  // the last tab area seen, for windows not yet shown
    bool m_attached = false;
    bool m_createRetried = false;   // session.create sent again once after a drop
    bool m_finished = false;
    bool m_followingFocus = false;  // taking the server's focus: not sent back
    std::function<QJsonObject()> m_keymapRequest;
    RexKeymap m_keymap;
    bool m_keymapPending = false;
    bool m_keymapAgain = false;  // fetch again once the pending one is in
    bool m_scriptsSent = false;  // scripts given to the server, to remove
    // Offline: the connection lost, the tabs kept.
    bool m_offline = false;
    QString m_offlineReason;
    int m_attempt = 0;          // reconnects tried since it was lost
    bool m_connecting = false;  // one of them under way
    qint64 m_retryAt = 0;       // ms since epoch; 0: not waiting
    QTimer *m_retryTimer = nullptr;
    QTimer *m_tickTimer = nullptr;  // the overlays' countdown
    bool m_restoring = false;
    QList<SavedWindow> m_restoreWindows;
};
