/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

/*
 * A saved place to connect to, listed in the Servers menu and edited on the
 * Servers page of Settings.
 *
 *   Rex:   a Rex server, http(s)://host:port; connecting asks which session.
 *   Tmux:  tmux on another machine over ssh: user@host and a session name
 *          (created if it doesn't exist).
 *   Vnc, Rdp: a remote desktop: host[:port], with a user name (RDP, or VNC
 *          servers that ask for one) and VNC's colour and quality choices.
 *   Web:   a page in a web tab.
 *
 * Passwords are never stored: remote desktops ask for them when they connect.
 */
struct SavedServer
{
    enum Kind { Rex, Tmux, Vnc, Rdp, Web };

    QString id;
    QString name;
    Kind kind = Rex;
    QString address;
    QString session;            // Tmux: the session name
    QString user;               // Vnc, Rdp
    bool reducedColour = false; // Vnc
    bool adaptive = false;      // Vnc

    /* What the menu calls it: the name, else the address. */
    QString title() const { return name.isEmpty() ? address : name; }

    static QString kindName(Kind kind);      // "Rex", "tmux over ssh", ...
    static QString kindKey(Kind kind);       // stored: "rex", "tmux", ...
    static Kind kindFromKey(const QString &key, bool *ok = nullptr);
};

/*
 * The saved servers, in the order shown, persisted in QSettings under
 * servers/list as JSON. Every change is written at once and announced with
 * changed().
 */
class ServerStore : public QObject
{
    Q_OBJECT

public:
    explicit ServerStore(QObject *parent = nullptr);

    QList<SavedServer> servers() const { return m_servers; }
    SavedServer server(const QString &id) const;

    /* Adds (a new id if it has none) or replaces the one with its id. */
    QString save(SavedServer server);
    void remove(const QString &id);
    /* Move one up (-1) or down (+1) the list. */
    void move(const QString &id, int step);

    /* The saved Rex server at this address, if there is one (for a name). */
    SavedServer rexServer(const QString &address) const;

signals:
    void changed();

private:
    void load();
    void write();

    QList<SavedServer> m_servers;
};
