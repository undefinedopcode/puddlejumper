/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QKeyCombination>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QKeyEvent;

/*
 * A Rex server's keymap for this client: what client.keymap answers.
 *
 * The client declares its actions and default bindings; the server merges
 * them with its own configuration (init.lua) and the client's overrides, and
 * answers with every binding, later layers first, so the first match wins.
 * Bindings may be sequences ("ctrl+a" then "c") and may belong to a mode
 * (entered with the client.mode.enter action); an exclusive mode swallows
 * keys it doesn't bind.
 *
 * Keys are spelled as rexia spells them: modifiers in the order ctrl, alt,
 * shift, cmd, then the key in lower case ("t", "enter", "esc", "pageup",
 * "f5", "left", "[", "space"). A stroke can have more than one spelling:
 * Shift+[ is both "shift+{" and "shift+[" (the US layout's unshifted key).
 * The Command key is "cmd", Control "ctrl", as rexia has them, so one
 * init.lua binds the same keys for both clients. (On macOS Qt reports
 * Command as Control and Control as Meta; elsewhere Meta is the Super key.)
 */
class RexKeymap
{
public:
    struct Binding
    {
        QStringList keys;
        QString action;
        QJsonObject args;
        QString mode;
        QString source;
    };
    struct Action
    {
        QString name;
        QString kind;  // "lua", "block", ...
        QString title;
        QString category;
        QString description;
        QString keywords;
    };
    enum class Result { Run, Prefix, Swallow, Unbound };

    /* Take client.keymap's answer. */
    void parse(const QJsonObject &reply);
    bool isLoaded() const { return m_loaded; }

    QString hash() const { return m_hash; }
    QString configPath() const { return m_configPath; }
    const QList<Binding> &bindings() const { return m_bindings; }
    /* The server's actions (configured and block ones), for menus. */
    const QList<Action> &actions() const { return m_actions; }
    const Action *action(const QString &name) const;
    QStringList diagnostics() const { return m_diagnostics; }
    bool hasMode(const QString &mode) const;

    /* What the strokes typed so far do in `mode` (empty: none): the first
     * binding that matches them all, skipping actions `available` rejects;
     * Prefix when they start a longer binding. */
    Result lookup(const QString &mode, const QList<QStringList> &typed,
                  const std::function<bool(const QString &)> &available, const Binding **found = nullptr) const;
    /* The keys first bound to an action outside every mode (with no
     * arguments, or `args` if given), or empty. */
    QStringList keysFor(const QString &action) const;

    /* A key event's spellings; empty for a lone modifier. */
    static QStringList spellings(const QKeyEvent *event);
    static QStringList spellings(int key, Qt::KeyboardModifiers modifiers);
    /* One key combination's main spelling (for the app's own shortcuts). */
    static QString spell(QKeyCombination combination);
    /* "ctrl+shift+t", "g" → "Ctrl+Shift+T G", for showing. */
    static QString display(const QStringList &keys);

private:
    bool m_loaded = false;
    QString m_hash;
    QString m_configPath;
    QList<Binding> m_bindings;
    QList<Action> m_actions;
    QStringList m_diagnostics;
    QHash<QString, bool> m_modes;  // name -> exclusive
};
