/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * OSC 6522, the Versatile Interface Adapter (puddle's docs/OSC6522.md): a
 * program in a Rex block describes its windows as widgets in the same
 * output as its cells, and the pane draws them as Qt widgets in place of
 * the cells. What the user does goes back to the program as pane input.
 *
 *   ViaScanner  finds the program's messages in the block's output.
 *   ViaClient   the terminal's side of the protocol, for one block: the
 *               foreground check, attach and its lease, frames in parts.
 *   ViaView     a program's windows as Qt widgets (the puddle.2 schema).
 *
 * The cells are still fed to the pane as ever; the view only covers them.
 */
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <map>

class QTimer;

class ViaScanner
{
public:
    // The bodies (after "6522;") of the messages in data, which may start
    // or end in the middle of one.
    QList<QByteArray> scan(const QByteArray &data);

private:
    QByteArray m_carry;  // the start of a message, or of its opener
};

class ViaClient : public QObject
{
    Q_OBJECT
public:
    // A window, as its last whole frame described it.
    struct Window
    {
        QString id;
        quint64 seq = 0;
        QJsonObject frame;  // title, modal, focus, root, state
    };

    explicit ViaClient(QObject *parent = nullptr);

    // How to check that pid is the block's foreground process.
    void setForegroundCheck(std::function<void(int pid, std::function<void(bool)> done)> check)
    {
        m_check = std::move(check);
    }

    // Program output, as the stream delivers it.
    void feed(const QByteArray &output);
    // The block's foreground process changed (0: unknown).
    void foregroundChanged(int pid);
    // The stream started again from a snapshot: frames may have been missed.
    void resynced();

    // Whether the native view should show: attached, with a window.
    bool active() const { return m_attached && !m_order.isEmpty(); }
    QString app() const { return m_app; }
    // Windows bottom to top.
    QList<const Window *> windows() const;

    // What the user did in window win (puddle.2 event data).
    void sendEvent(const QString &win, const QJsonObject &data);
    // Back to the cells: detach, and stay detached until the program
    // starts again.
    void detach();

signals:
    // Bytes for the program's input. user: the user did something.
    void send(const QByteArray &bytes, bool user);
    void windowsChanged();
    void ended();

private:
    void message(const QByteArray &body);
    void hello(const QHash<QString, QString> &kv);
    void attach(bool full);
    void end();

    ViaScanner m_scanner;
    std::function<void(int, std::function<void(bool)>)> m_check;
    QTimer *m_renew = nullptr;
    QString m_client;
    QString m_inst;      // the program's, once attached
    QString m_declined;  // an inst the user went back to the cells from
    QString m_app;
    int m_pid = 0;
    int m_max = 4096;    // the longest sequence the program takes
    bool m_attached = false;
    bool m_checking = false;
    quint64 m_events = 0;
    std::map<QString, Window> m_windows;
    QStringList m_order;  // bottom to top
    // A frame coming in parts.
    QString m_partWin;
    quint64 m_partSeq = 0;
    int m_partNext = 0;
    QByteArray m_partData;
};

class ViaView : public QWidget
{
    Q_OBJECT
public:
    ViaView(ViaClient *client, QWidget *parent = nullptr);

    // Colours to draw with (the pane's).
    void setColours(const QColor &background, const QColor &foreground, const QColor &accent);

signals:
    // The user asked for the cells back.
    void cellsRequested();

private:
    void rebuild();
    QWidget *build(const QString &win, const QJsonObject &node, const QJsonObject &state);
    void restyle();

    QPointer<ViaClient> m_client;
    QWidget *m_body = nullptr;
    QString m_built;  // the frames the widgets were built from
    QColor m_bg, m_fg, m_accent;
};
