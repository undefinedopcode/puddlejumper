/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QStringList>

#include <functional>

struct PaneLayout;

/*
 * A tmux control-mode client: the protocol, without any idea of windows or
 * widgets (TmuxSession maps those).
 *
 * Runs `tmux -C …` (possibly behind ssh) with plain pipes. tmux then speaks a
 * line protocol on stdout: replies to our commands framed by %begin and
 * %end/%error, and notifications (%output, %layout-change, %window-add …)
 * between them. Commands are written to stdin one per line and answered in
 * order, so replies are matched to callbacks with a queue.
 */
class TmuxControl : public QObject
{
    Q_OBJECT

public:
    using Reply = std::function<void(bool ok, const QList<QByteArray> &lines)>;

    explicit TmuxControl(QObject *parent = nullptr);
    ~TmuxControl() override;

    /* command: program and arguments, e.g. {"tmux", "-C", "attach"}. */
    void start(const QStringList &command);
    bool isRunning() const;

    /* Send one tmux command; reply, if given, runs when tmux answers it. */
    void command(const QString &cmd, Reply reply = {});

    /* Detach: tmux keeps the session, this client exits. */
    void detach();

    /* Undo %output's escaping: \ooo octal for control bytes and backslash. */
    static QByteArray unescape(const QByteArray &text);
    /* Parse a window layout, classic ("b25d,80x24,0,0{40x24,0,0,1,39x24,41,0,2}")
     * or tmux 3.8's JSON ({"V":2,"L":{…}}). */
    static bool parseLayout(const QByteArray &text, PaneLayout &out);

signals:
    /* The client is up (tmux finished its startup command). */
    void ready();
    /* The client has gone: detached, killed, or failed to start. */
    void exited(const QString &reason);

    void output(int pane, const QByteArray &data);
    void windowAdded(int window);
    void windowClosed(int window);
    void windowRenamed(int window, const QString &name);
    void layoutChanged(int window, const QByteArray &layout);
    void activeWindowChanged(int window);
    void activePaneChanged(int window, int pane);
    void sessionChanged(const QString &name);
    /* Flow control (refresh-client -f pause-after): output for `pane` was
     * dropped because we fell behind, and it waits for "continue". */
    void paused(int pane);

private:
    void readOutput();
    void handleLine(QByteArray line);
    void handleNotification(const QByteArray &line);
    void finishBlock(bool ok);

    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    QQueue<Reply> m_pending;
    bool m_ready = false;
    bool m_exited = false;
    QString m_exitReason;

    // The reply block being read, if any.
    bool m_inBlock = false;
    bool m_blockIsOurs = false;
    QByteArray m_blockNumber;
    QList<QByteArray> m_blockLines;
};
