/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "profiles.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QUrl>

#include <functional>
#include <map>
#include <memory>

class QTimer;
class RexControl;
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
 * Detaching leaves the session running, to be attached again from any Rex
 * client. Floating layers aren't shown yet; their blocks run on unseen.
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

    QList<TerminalTab *> tabs() const;
    bool ownsTab(const Tab *tab) const;
    bool ownsPane(const TerminalWidget *pane) const;
    QString windowName(const TerminalTab *tab) const;
    QString blockId(const TerminalWidget *pane) const;
    TerminalWidget *paneForBlock(const QString &blockId) const;
    /* Whether this pane's program has exited (its block kept to be read). */
    bool hasExited(const TerminalWidget *pane) const;

    void newWindow();
    void splitPane(TerminalWidget *pane, Qt::Orientation orientation);
    void closePane(TerminalWidget *pane);
    void closeWindow(TerminalTab *tab);
    void renameWindow(TerminalTab *tab, const QString &name);

    /* The sessions on a server, for a menu: calls back with (id, label)
     * pairs, or an error. */
    static void listSessions(const QUrl &server, QObject *context,
                             std::function<void(const QList<QPair<QString, QString>> &, const QString &error)> done);

signals:
    /* A window appeared; the receiver shows `tab`. */
    void tabAdded(TerminalTab *tab, const QString &name);
    void tabRenamed(TerminalTab *tab, const QString &name);
    /* A window went; the receiver takes the tab away and deletes it. */
    void tabRemoved(TerminalTab *tab);
    /* Gone: detached, destroyed, emptied, or the connection lost for good.
     * All tabs have been removed already. `reason` may be empty. */
    void ended(const QString &reason);

private:
    struct Block
    {
        QString id;
        int paneId = -1;
        QPointer<TerminalWidget> pane;
        RexTerminalConnection *stream = nullptr;
        QSize screen;  // the block's size, as the server last said
        QSize sent;    // the size last asked for
        int resizing = 0;  // resize requests not yet answered
        int retries = 0;   // asked again, the server having kept another size
        bool exited = false;
        int exitCode = 0;
    };
    struct Window
    {
        QString id;
        QString label;
        QPointer<TerminalTab> tab;
        QJsonObject layout;  // the tiled layer's layout tree
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
    void sendInput(Block *b, const QByteArray &bytes);
    void sendSize(Block *b, const QSize &cells, bool again = false);
    void checkSize(Block *b);
    void sendTheme(Block *b);
    void splitterDragged(Window &w, TerminalWidget *before, Qt::Orientation orientation, int pixels);
    void finish(const QString &reason);
    void endIfEmpty();
    Window *windowOf(const TerminalTab *tab);
    const Window *windowOf(const TerminalTab *tab) const;
    QJsonObject target(const Block *b) const;
    QSize cellSize();
    QSize paneChrome() const;

    QUrl m_server;
    QString m_sessionId;
    QString m_label;
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
    bool m_finished = false;
};
