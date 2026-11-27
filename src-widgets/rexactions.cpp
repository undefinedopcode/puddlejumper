/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * MainWindow: actions by name, the command palette, and Rex keymaps.
 *
 * The app's actions carry Rex names (rexia's, where rexia has the same
 * action: "client.tab.new", "pane.split.right", ...). A Rex session sends
 * them, with their shortcuts as default bindings, in client.keymap; the
 * server merges in its init.lua and answers with the keymap that holds in
 * that session's panes. There, keys go by it: a binding to one of the app's
 * actions runs it here, one to the server's (a Lua action, the terminal's
 * clear and reset) is run there with client.action. A key the app would
 * otherwise take but the keymap doesn't bind (init.lua unbound it) goes to
 * the terminal. Until a keymap arrives, and in other tabs, the app's
 * shortcuts work as ever.
 *
 * A binding to client.key.forward is a key handler the client's own scripts
 * (client.set_script) gave the server: the key goes there with client.key.
 *
 * The server can also ask this client to run one of its actions
 * (client_perform_requested: another client's client.perform, or an action's
 * rex.perform), and an action can ask a question (client_prompt_requested,
 * shown as a dialog).
 */

#include "mainwindow.h"

#include "commandpalette.h"
#include "notifier.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#ifdef PUDDLEJUMPER_REX
#include "rexkeymap.h"
#include "rexprompt.h"
#include "rexsession.h"

#include <QFileInfo>

#include <memory>
#endif

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

QString plainTitle(const QAction *a)
{
    QString t = a->text();
    t.remove(QLatin1Char('&'));
    if (t.endsWith(QStringLiteral("…")))
        t.chop(1);
    return t.section(QLatin1Char('\t'), 0, 0).trimmed();
}

// A JSON schema for an action's arguments: an object of these properties.
QJsonObject argsSchema(const QList<std::pair<QString, QString>> &properties, const QStringList &required = {})
{
    QJsonObject props;
    for (const auto &[name, type] : properties)
        props.insert(name, QJsonObject{{QStringLiteral("type"), type}});
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), props},
                       {QStringLiteral("additionalProperties"), false}};
    if (!required.isEmpty())
        schema.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
    return schema;
}

// Actions that are the app's but not a menu's: they need arguments.
bool argumentAction(const QString &name)
{
    return name.startsWith(QLatin1String("client.mode.")) || name == QLatin1String("client.tab.goto")
        || name == QLatin1String("client.notify") || name == QLatin1String("client.clipboard.set")
        || name == QLatin1String("client.workspace.switch");
}

QJsonObject apiError(const QString &code, const QString &message)
{
    return QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

}  // namespace

QList<QAction *> MainWindow::namedActions() const
{
    QList<QAction *> actions = this->actions() + menuBar()->actions();
    for (qsizetype i = 0; i < actions.size(); ++i) {
        if (QMenu *menu = actions.at(i)->menu())
            actions += menu->actions();
    }
    QList<QAction *> out;
    for (QAction *a : std::as_const(actions)) {
        if (!a->property("rexAction").toString().isEmpty() && !out.contains(a))
            out << a;
    }
    return out;
}

void MainWindow::showCommandPalette()
{
    QList<CommandPalette::Entry> entries;
    QPointer<TerminalWidget> pane(currentTerminal());
#ifdef PUDDLEJUMPER_REX
    RexSession *rex = pane ? rexSessionFor(pane.data()) : nullptr;
#endif
    for (QAction *a : namedActions()) {
        const QString name = a->property("rexAction").toString();
        if (name == QLatin1String("client.palette.toggle") || !a->isEnabled())
            continue;
        QString keys = a->shortcut().toString(QKeySequence::NativeText);
#ifdef PUDDLEJUMPER_REX
        if (rex && rex->keymap().isLoaded())
            keys = RexKeymap::display(rex->keymap().keysFor(name));
#endif
        QPointer<QAction> action(a);
        entries << CommandPalette::Entry{plainTitle(a), a->property("rexCategory").toString(), name, {}, keys,
                                         [this, action, pane]() {
                                             if (pane)
                                                 pane->setFocus();
                                             if (action)
                                                 action->trigger();
                                         }};
    }
#ifdef PUDDLEJUMPER_REX
    if (rex) {
        QPointer<RexSession> session(rex);
        for (const RexKeymap::Action &a : rex->keymap().peopleActions()) {
            const QString name = a.name;
            // Rex's own group for one with none (an older server's).
            entries << CommandPalette::Entry{a.title, a.category.isEmpty() ? tr("Actions") : a.category, name,
                                             a.keywords + QLatin1Char(' ') + a.description,
                                             RexKeymap::display(rex->keymap().keysFor(name)),
                                             [session, pane, name]() {
                                                 if (session)
                                                     session->runAction(name, {}, pane, QStringLiteral("palette"));
                                             }};
        }
    }
#endif
    auto *palette = new CommandPalette(entries, this);
    palette->open();
}

#ifdef PUDDLEJUMPER_REX

QJsonObject MainWindow::rexKeymapRequest() const
{
    QJsonArray actions, bindings;
    for (const QAction *a : namedActions()) {
        const QString name = a->property("rexAction").toString();
        QJsonObject action{{QStringLiteral("name"), name}, {QStringLiteral("kind"), QStringLiteral("client")},
                           {QStringLiteral("title"), plainTitle(a)},
                           {QStringLiteral("category"), a->property("rexCategory").toString()}};
        if (name == QLatin1String("client.tab.rename"))
            action.insert(QStringLiteral("args"), argsSchema({{QStringLiteral("title"), QStringLiteral("string")}}));
        actions << action;
        for (const QKeySequence &seq : a->shortcuts()) {
            QJsonArray keys;
            for (int i = 0; i < seq.count(); ++i) {
                const QString k = RexKeymap::spell(seq[i]);
                if (!k.isEmpty())
                    keys << k;
            }
            if (!keys.isEmpty() && keys.size() == seq.count())
                bindings << QJsonObject{{QStringLiteral("keys"), keys}, {QStringLiteral("action"), name}};
        }
    }
    // Actions that need arguments, so only for bindings (and scripts).
    actions << QJsonObject{{QStringLiteral("name"), QStringLiteral("client.tab.goto")},
                           {QStringLiteral("kind"), QStringLiteral("client")},
                           {QStringLiteral("title"), tr("Go to Tab")},
                           {QStringLiteral("description"), tr("The tab at index (1 is the first, -1 the last)")},
                           {QStringLiteral("category"), QStringLiteral("Tab")},
                           {QStringLiteral("args"), argsSchema({{QStringLiteral("index"), QStringLiteral("integer")}},
                                                               {QStringLiteral("index")})}};
    actions << QJsonObject{{QStringLiteral("name"), QStringLiteral("client.workspace.switch")},
                           {QStringLiteral("kind"), QStringLiteral("client")},
                           {QStringLiteral("title"), tr("Switch to Workspace")},
                           {QStringLiteral("description"), tr("The workspace called name, or at index (1 is the first)")},
                           {QStringLiteral("category"), QStringLiteral("Workspace")},
                           {QStringLiteral("args"), argsSchema({{QStringLiteral("name"), QStringLiteral("string")},
                                                                {QStringLiteral("index"), QStringLiteral("integer")}})}};
    actions << QJsonObject{{QStringLiteral("name"), QStringLiteral("client.notify")},
                           {QStringLiteral("kind"), QStringLiteral("client")},
                           {QStringLiteral("title"), tr("Show a Notification")},
                           {QStringLiteral("description"),
                            tr("A desktop notification: title, and body if given, naming the tab it came from")},
                           {QStringLiteral("category"), QStringLiteral("App")},
                           {QStringLiteral("args"), argsSchema({{QStringLiteral("title"), QStringLiteral("string")},
                                                                {QStringLiteral("body"), QStringLiteral("string")}},
                                                               {QStringLiteral("title")})}};
    actions << QJsonObject{{QStringLiteral("name"), QStringLiteral("client.clipboard.set")},
                           {QStringLiteral("kind"), QStringLiteral("client")},
                           {QStringLiteral("title"), tr("Copy to the Clipboard")},
                           {QStringLiteral("description"), tr("Puts text on this computer's clipboard")},
                           {QStringLiteral("category"), QStringLiteral("Edit")},
                           {QStringLiteral("args"), argsSchema({{QStringLiteral("text"), QStringLiteral("string")}},
                                                               {QStringLiteral("text")})}};
    // Keymap modes: entered and left by binding these.
    for (const char *name : {"client.mode.enter", "client.mode.exit", "client.mode.exit_all"}) {
        const QString n = QString::fromLatin1(name);
        actions << QJsonObject{{QStringLiteral("name"), n}, {QStringLiteral("kind"), QStringLiteral("client")},
                               {QStringLiteral("title"), n == QLatin1String("client.mode.enter") ? tr("Enter Mode")
                                                         : n == QLatin1String("client.mode.exit") ? tr("Exit Mode")
                                                                                                  : tr("Exit All Modes")},
                               {QStringLiteral("category"), QStringLiteral("Keys")}};
    }
    return QJsonObject{{QStringLiteral("actions"), actions},
                       {QStringLiteral("defaults"),
                        QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("puddlejumper")},
                                               {QStringLiteral("bindings"), bindings}}}}};
}

void MainWindow::resendRexScripts()
{
    const QString file = m_settings.rexScriptsFile();
    if (file.isEmpty()) {
        statusBar()->showMessage(tr("No Rex scripts file is set (Settings › Terminal › Rex)"), 6000);
        return;
    }
    if (m_rexSessions.isEmpty()) {
        statusBar()->showMessage(tr("No Rex session is open to send %1 to").arg(QFileInfo(file).fileName()), 6000);
        return;
    }
    // What each session's server says, then one message for them all.
    struct Tally
    {
        int pending = 0, sent = 0;
        QStringList problems;
    };
    auto tally = std::make_shared<Tally>();
    tally->pending = int(m_rexSessions.size());
    const QString name = QFileInfo(file).fileName();
    QPointer<MainWindow> self(this);
    statusBar()->showMessage(tr("Sending %1…").arg(name));
    for (RexSession *s : std::as_const(m_rexSessions)) {
        s->sendScripts([self, tally, name](bool ok, const QString &message) {
            if (ok)
                ++tally->sent;
            else
                tally->problems << message;
            if (--tally->pending > 0 || !self)
                return;
            if (tally->problems.isEmpty())
                self->statusBar()->showMessage(tr("Sent %1 to %n Rex session(s)", nullptr, tally->sent).arg(name), 5000);
            else
                self->statusBar()->showMessage(
                    tr("%1 not sent: %2").arg(name, tally->problems.first())
                        + (tally->sent ? tr(" (sent to %n other(s))", nullptr, tally->sent) : QString()),
                    12000);
        });
    }
}

void MainWindow::wireRexActions(RexSession *session)
{
    session->setKeymapRequest([this]() { return rexKeymapRequest(); });
    // The scripts file (Settings > Terminal > Rex), sent again when it changes.
    connect(&m_settings, &Settings::rexChanged, session, [session]() { session->sendScripts(); });
    connect(session, &RexSession::keymapChanged, this, [this, session]() {
        const QStringList d = session->keymap().diagnostics();
        if (!d.isEmpty()) {
            statusBar()->showMessage(tr("Rex keymap: %1").arg(d.first())
                                         + (d.size() > 1 ? tr(" (and %n more)", nullptr, int(d.size() - 1)) : QString()),
                                     10000);
        }
        if (session == m_rexKeySession) {
            m_rexTyped.clear();
            if (!session->keymap().hasMode(m_rexMode))
                m_rexMode.clear();
        }
    });
    connect(session, &RexSession::performRequested, this,
            [this, session](const QString &callId, const QString &action, const QJsonObject &args, TerminalWidget *pane) {
                QString why;
                if (performClientAction(action, args, pane, &why, session))
                    session->replyCall(callId, QJsonValue());
                else
                    session->replyCall(callId, {}, apiError(QStringLiteral("not_supported"), why));
            });
    connect(session, &RexSession::promptRequested, this,
            [this, session](const QString &callId, const QJsonObject &prompt) { showRexPrompt(session, callId, prompt); });
    connect(session, &RexSession::promptEnded, this, [this, session](const QString &callId) {
        if (m_rexPrompt && m_rexPromptSession == session && m_rexPromptCall == callId) {
            m_rexPromptCall.clear();  // closed without an answer
            m_rexPrompt->reject();
        }
    });
    // What an action gives back, if anything, for a few seconds.
    connect(session, &RexSession::actionFinished, this, [this, session](const QString &action, const QJsonValue &result) {
        QString text;
        if (result.isString())
            text = result.toString();
        else if (result.isBool() || result.isDouble())
            text = result.toVariant().toString();
        else if (result.isObject() || result.isArray())
            text = QString::fromUtf8(result.isObject() ? QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact)
                                                       : QJsonDocument(result.toArray()).toJson(QJsonDocument::Compact));
        text = text.simplified();
        if (text.isEmpty() || text == QLatin1String("{}") || text == QLatin1String("[]"))
            return;
        if (text.size() > 200)
            text = text.left(200) + QStringLiteral("…");
        const RexKeymap::Action *a = session->keymap().action(action);
        statusBar()->showMessage(tr("%1: %2").arg(a ? a->title : action, text), 6000);
    });
    connect(session, &RexSession::actionFailed, this, [this](const QString &action, const QString &message) {
        statusBar()->showMessage(tr("%1: %2").arg(action, message), 8000);
    });
}

bool MainWindow::performClientAction(const QString &name, const QJsonObject &args, TerminalWidget *pane,
                                     QString *error, RexSession *from)
{
    // Where an action came from (the pane's tab, else its Rex session), so
    // what it does can say so: a script can't pass for something else.
    TerminalTab *sourceTab = nullptr;
    const auto source = [&]() {
        for (Tab *page : allPages()) {
            if (auto *t = qobject_cast<TerminalTab *>(page); t && pane && t->contains(pane))
                sourceTab = t;
        }
#ifdef PUDDLEJUMPER_REX
        if (!from && pane)
            from = rexSessionFor(pane);
#endif
        QString where = sourceTab ? describeTab(sourceTab) : QString();
#ifdef PUDDLEJUMPER_REX
        if (!sourceTab && from)
            where = tr("Rex %1").arg(from->label());
#endif
        return where.isEmpty() ? tr("Rex") : where;
    };
    if (name == QLatin1String("client.notify")) {
        const QString title = args.value(QLatin1String("title")).toString().simplified();
        const QString body = args.value(QLatin1String("body")).toString().trimmed();
        if (title.isEmpty()) {
            *error = tr("client.notify needs a title");
            return false;
        }
        const QString where = source();
        // Clicking it shows the pane's tab; it goes when that tab is seen.
        const QString key = sourceTab ? notificationKey(sourceTab)
                                      : QStringLiteral("notify-%1").arg(quintptr(from), 0, 16);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_scriptNotifiedAt.value(key) < 1000) {
            *error = tr("Too many notifications: one a second");
            return false;
        }
        m_scriptNotifiedAt.insert(key, now);
        m_notifier->notify(key, tr("%1 — %2").arg(title.left(200), where), body.left(2000));
        return true;
    }
    if (name == QLatin1String("client.clipboard.set")) {
        const QString text = args.value(QLatin1String("text")).toString();
        if (text.isEmpty()) {
            *error = tr("client.clipboard.set needs text");
            return false;
        }
        constexpr qsizetype kMaxClipboard = 512 * 1024;
        if (text.size() > kMaxClipboard) {
            *error = tr("Too much text for the clipboard: %1 characters, at most %2")
                         .arg(text.size())
                         .arg(kMaxClipboard);
            return false;
        }
        QApplication::clipboard()->setText(text);
        // Said, always: what's pasted next is no longer what was copied.
        statusBar()->showMessage(tr("Copied to the clipboard by %1: %n character(s)", nullptr, int(text.size()))
                                     .arg(source()),
                                 8000);
        return true;
    }
    if (name == QLatin1String("client.mode.enter")) {
        const QString mode = args.value(QLatin1String("mode")).toString();
        if (mode.isEmpty()) {
            *error = tr("client.mode.enter needs a mode");
            return false;
        }
        m_rexMode = mode;
        statusBar()->showMessage(tr("Mode: %1 (Esc leaves it)").arg(mode));
        return true;
    }
    if (name == QLatin1String("client.mode.exit") || name == QLatin1String("client.mode.exit_all")) {
        m_rexMode.clear();
        statusBar()->clearMessage();
        return true;
    }
    if (name == QLatin1String("client.tab.goto")) {
        const QJsonValue v = args.value(QLatin1String("index"));
        const int n = m_tabs->count();
        const int index = v.toInt(0);
        if (!v.isDouble() || index == 0 || v.toDouble() != index) {
            *error = tr("client.tab.goto needs an index (1 is the first tab, -1 the last)");
            return false;
        }
        const int i = index > 0 ? index - 1 : n + index;
        if (i < 0 || i >= n) {
            *error = tr("There is no tab %1 (this workspace has %n)", nullptr, n).arg(index);
            return false;
        }
        m_tabs->setCurrentIndex(i);
        return true;
    }
    if (name == QLatin1String("client.workspace.switch")) {
        int found = -1;
        const QJsonValue byName = args.value(QLatin1String("name")), byIndex = args.value(QLatin1String("index"));
        if (byName.isString()) {
            const QString want = byName.toString().trimmed();
            for (int i = 0; i < m_workspaces.size() && found < 0; ++i) {
                if (m_workspaces.at(i).name == want)
                    found = i;
            }
            for (int i = 0; i < m_workspaces.size() && found < 0; ++i) {
                if (m_workspaces.at(i).name.compare(want, Qt::CaseInsensitive) == 0)
                    found = i;
            }
            if (found < 0) {
                *error = tr("There is no workspace “%1”").arg(want);
                return false;
            }
        } else if (byIndex.isDouble()) {
            const int index = byIndex.toInt(0);
            if (index < 1 || index > m_workspaces.size() || byIndex.toDouble() != index) {
                *error = tr("There is no workspace %1 (there are %n)", nullptr, int(m_workspaces.size()))
                             .arg(byIndex.toDouble());
                return false;
            }
            found = index - 1;
        } else {
            *error = tr("client.workspace.switch needs a name or an index");
            return false;
        }
        switchWorkspace(found);   // nothing to do if it's on screen already
        return true;
    }
    if (name == QLatin1String("client.tab.rename") && args.contains(QLatin1String("title"))) {
        const QJsonValue title = args.value(QLatin1String("title"));
        if (!title.isString()) {
            *error = tr("client.tab.rename's title is text");
            return false;
        }
        // The tab of the pane the request names, else the one on screen.
        int index = m_tabs->currentIndex();
        for (int i = 0; pane && i < m_tabs->count(); ++i) {
            if (auto *t = qobject_cast<TerminalTab *>(m_tabs->widget(i)); t && t->contains(pane))
                index = i;
        }
        setTabName(index, title.toString().trimmed());
        return true;
    }
    for (QAction *a : namedActions()) {
        if (a->property("rexAction").toString() != name)
            continue;
        if (!a->isEnabled()) {
            *error = tr("%1 can't run now").arg(name);
            return false;
        }
        // On the pane the request names, if any.
        if (pane && pane != currentTerminal()) {
            for (int i = 0; i < m_tabs->count(); ++i) {
                if (auto *t = qobject_cast<TerminalTab *>(m_tabs->widget(i)); t && t->contains(pane)) {
                    m_tabs->setCurrentIndex(i);
                    break;
                }
            }
            pane->setFocus();
        }
        a->trigger();
        return true;
    }
    *error = tr("PuddleJumper has no action %1").arg(name);
    return false;
}

void MainWindow::runRexAction(RexSession *session, TerminalWidget *pane, const QString &name, const QJsonObject &args,
                              const QString &origin)
{
    QString why;
    const bool ours = argumentAction(name) || [&] {
        for (QAction *a : namedActions())
            if (a->property("rexAction").toString() == name)
                return true;
        return false;
    }();
    if (ours) {
        if (!performClientAction(name, args, pane, &why, session))
            statusBar()->showMessage(why, 5000);
        return;
    }
    session->runAction(name, args, pane, origin);
}

bool MainWindow::routeRexKey(QKeyEvent *event)
{
    // Only keys for a Rex pane in this window, once its keymap is here.
    auto *focus = qobject_cast<QWidget *>(QApplication::focusWidget());
    if (!focus || focus->window() != this)
        return false;
    TerminalWidget *pane = nullptr;
    for (QWidget *w = focus; w && !pane; w = w->parentWidget())
        pane = qobject_cast<TerminalWidget *>(w);
    RexSession *rex = pane ? rexSessionFor(pane) : nullptr;
    if (!rex || !rex->keymap().isLoaded())
        return false;
    const QStringList spellings = RexKeymap::spellings(event);
    if (spellings.isEmpty())
        return false;  // a modifier on its own
    if (rex != m_rexKeySession) {
        m_rexKeySession = rex;
        m_rexTyped.clear();
        m_rexMode.clear();
    }

    const RexKeymap &keymap = rex->keymap();
    const auto available = [this, &keymap](const QString &action) {
        if (action.startsWith(QLatin1String("client.mode.")) || action == QLatin1String("client.key.forward")
            || keymap.action(action))
            return true;
        for (QAction *a : namedActions())
            if (a->property("rexAction").toString() == action)
                return a->isEnabled();
        return false;
    };
    QList<QStringList> typed = m_rexTyped;
    typed << spellings;
    const RexKeymap::Binding *binding = nullptr;
    const RexKeymap::Result result = keymap.lookup(m_rexMode, typed, available, &binding);
    const bool leaveMode = result == RexKeymap::Result::Unbound && !m_rexMode.isEmpty()
                           && event->key() == Qt::Key_Escape && m_rexTyped.isEmpty();
    const bool breakSequence = result == RexKeymap::Result::Unbound && !m_rexTyped.isEmpty();

    if (event->type() == QEvent::ShortcutOverride) {
        // Taken here, the app's shortcuts don't fire. One the keymap doesn't
        // bind (init.lua unbound it) is the terminal's.
        if (result != RexKeymap::Result::Unbound || leaveMode || breakSequence || isAppShortcut(event)) {
            event->accept();
            return true;
        }
        return false;
    }

    switch (result) {
    case RexKeymap::Result::Run: {
        m_rexTyped.clear();
        if (!m_rexMode.isEmpty() || typed.size() > 1)
            statusBar()->clearMessage();
        const QString action = binding->action;
        const QJsonObject args = binding->args;
        if (action == QLatin1String("client.key.forward")) {
            // The server's: its handler runs, or it types the key.
            rex->forwardKey(pane, RexKeymap::keyEvent(event), binding->keys.mid(0, binding->keys.size() - 1));
            return true;
        }
        runRexAction(rex, pane, action, args, QStringLiteral("key"));
        if (!m_rexMode.isEmpty())
            statusBar()->showMessage(tr("Mode: %1 (Esc leaves it)").arg(m_rexMode));
        return true;
    }
    case RexKeymap::Result::Prefix: {
        m_rexTyped = typed;
        QStringList shown;
        for (const QStringList &s : std::as_const(m_rexTyped))
            shown << s.value(0);
        statusBar()->showMessage(RexKeymap::display(shown) + QStringLiteral(" …"));
        return true;
    }
    case RexKeymap::Result::Swallow:
        m_rexTyped.clear();
        return true;
    case RexKeymap::Result::Unbound:
        break;
    }
    if (leaveMode) {
        m_rexMode.clear();
        statusBar()->clearMessage();
        return true;
    }
    if (breakSequence) {
        // A key that ends no binding: the sequence is dropped, and so is it.
        m_rexTyped.clear();
        statusBar()->clearMessage();
        return true;
    }
    return false;  // the terminal's
}

void MainWindow::showRexPrompt(RexSession *session, const QString &callId, const QJsonObject &prompt)
{
    if (m_rexPrompt) {
        session->replyCall(callId, {}, apiError(QStringLiteral("not_supported"),
                                                tr("PuddleJumper shows one prompt at a time")));
        return;
    }
    QPointer<RexSession> from(session);
    auto answer = [this, from](const QJsonObject &result) {
        const QString call = std::exchange(m_rexPromptCall, QString());
        if (from && !call.isEmpty())
            from->replyCall(call, result);
    };
    // A palette over this window (rexprompt.h), as Rex's apps ask.
    auto *palette = new RexPrompt(prompt, this);
    palette->setAttribute(Qt::WA_DeleteOnClose);
    connect(palette, &QDialog::accepted, this, [palette, answer]() { answer(palette->answer()); });
    connect(palette, &QDialog::rejected, this, [answer]() { answer(QJsonObject{{QStringLiteral("cancelled"), true}}); });
    m_rexPrompt = palette;
    m_rexPromptCall = callId;
    m_rexPromptSession = session;
    palette->open();
}

#endif  // PUDDLEJUMPER_REX
