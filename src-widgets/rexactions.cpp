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
 * The server can also ask this client to run one of its actions
 * (client_perform_requested: another client's client.perform, or an action's
 * rex.perform), and an action can ask a question (client_prompt_requested,
 * shown as a dialog).
 */

#include "mainwindow.h"

#include "commandpalette.h"
#include "terminaltab.h"
#include "terminalwidget.h"

#ifdef PUDDLEJUMPER_REX
#include "rexkeymap.h"
#include "rexsession.h"
#endif

#include <QAction>
#include <QApplication>
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
        for (const RexKeymap::Action &a : rex->keymap().actions()) {
            const QString name = a.name;
            entries << CommandPalette::Entry{a.title, a.category.isEmpty() ? tr("Rex") : a.category, name,
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
        actions << QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("kind"), QStringLiteral("client")},
                               {QStringLiteral("title"), plainTitle(a)},
                               {QStringLiteral("category"), a->property("rexCategory").toString()}};
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

void MainWindow::wireRexActions(RexSession *session)
{
    session->setKeymapRequest([this]() { return rexKeymapRequest(); });
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
                if (performClientAction(action, args, pane, &why))
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
                                     QString *error)
{
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
    const bool ours = name.startsWith(QLatin1String("client.mode.")) || [&] {
        for (QAction *a : namedActions())
            if (a->property("rexAction").toString() == name)
                return true;
        return false;
    }();
    if (ours) {
        if (!performClientAction(name, args, pane, &why))
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
        if (action.startsWith(QLatin1String("client.mode.")) || keymap.action(action))
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
    const QString title = prompt.value(QLatin1String("title")).toString();
    QPointer<RexSession> from(session);
    auto answer = [this, from](const QString &call, const QJsonObject &result) {
        if (from && !call.isEmpty())
            from->replyCall(call, result);
    };
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(title, dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    const QString confirm = prompt.value(QLatin1String("confirm")).toString();
    if (!confirm.isEmpty())
        buttons->button(QDialogButtonBox::Ok)->setText(confirm);

    QLineEdit *edit = nullptr;
    QListWidget *list = nullptr;
    const bool pick = prompt.value(QLatin1String("kind")).toString() == QLatin1String("pick");
    const bool multiple = prompt.value(QLatin1String("multiple")).toBool();
    if (pick) {
        list = new QListWidget(dialog);
        list->setSelectionMode(multiple ? QAbstractItemView::MultiSelection : QAbstractItemView::SingleSelection);
        for (const QJsonValue &v : prompt.value(QLatin1String("items")).toArray()) {
            const QJsonObject it = v.toObject();
            const QString id = it.value(QLatin1String("id")).toVariant().toString();
            QString text = it.value(QLatin1String("title")).toString(id);
            if (const QString sub = it.value(QLatin1String("subtitle")).toString(); !sub.isEmpty())
                text += QStringLiteral(" — ") + sub;
            auto *item = new QListWidgetItem(text, list);
            item->setData(Qt::UserRole, it.value(QLatin1String("id")).toVariant());
            item->setToolTip(it.value(QLatin1String("detail")).toString());
            item->setSelected(it.value(QLatin1String("selected")).toBool());
        }
        if (!multiple && list->count() && list->selectedItems().isEmpty())
            list->setCurrentRow(0);
        layout->addWidget(list);
        connect(list, &QListWidget::itemActivated, dialog, &QDialog::accept);
    }
    if (!pick || prompt.value(QLatin1String("allow_other")).toBool()) {
        edit = new QLineEdit(prompt.value(QLatin1String("value")).toString(), dialog);
        edit->setPlaceholderText(prompt.value(QLatin1String("placeholder")).toString(
            pick ? tr("Or type another answer") : QString()));
        layout->addWidget(edit);
        edit->setFocus();
    }
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, edit, list, pick, answer]() {
        const QString call = std::exchange(m_rexPromptCall, QString());
        if (pick && !(edit && !edit->text().trimmed().isEmpty())) {
            QJsonArray ids;
            for (const QListWidgetItem *item : list->selectedItems())
                ids << QJsonValue::fromVariant(item->data(Qt::UserRole));
            if (ids.isEmpty() && list->currentItem())
                ids << QJsonValue::fromVariant(list->currentItem()->data(Qt::UserRole));
            answer(call, QJsonObject{{QStringLiteral("ids"), ids}});
        } else {
            answer(call, QJsonObject{{QStringLiteral("text"), edit ? edit->text() : QString()}});
        }
    });
    connect(dialog, &QDialog::rejected, this, [this, answer]() {
        const QString call = std::exchange(m_rexPromptCall, QString());
        answer(call, QJsonObject{{QStringLiteral("cancelled"), true}});
    });
    m_rexPrompt = dialog;
    m_rexPromptCall = callId;
    m_rexPromptSession = session;
    dialog->open();
}

#endif  // PUDDLEJUMPER_REX
