/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexkeymap: RexKeymap on its own: how key events are spelled, and what
 * a keymap (as a Rex server's client.keymap answers) does with them.
 */

#include "rexkeymap.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QTextStream>

namespace {

int failures = 0;
int checks = 0;

bool check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL: " << what << "\n";
        ++failures;
    }
    return ok;
}

// The keys rexia calls ctrl and cmd, as Qt reports them.
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier Ctrl = Qt::MetaModifier;
constexpr Qt::KeyboardModifier Cmd = Qt::ControlModifier;
const QString kShortcutMod = QStringLiteral("cmd");  // a QKeySequence's "Ctrl"
#else
constexpr Qt::KeyboardModifier Ctrl = Qt::ControlModifier;
constexpr Qt::KeyboardModifier Cmd = Qt::MetaModifier;
const QString kShortcutMod = QStringLiteral("ctrl");
#endif

QStringList spell(int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent e(QEvent::KeyPress, key, mods);
    return RexKeymap::spellings(&e);
}

QJsonObject keymapJson()
{
    return QJsonDocument::fromJson(R"({
      "bindings": [
        {"keys": ["ctrl+shift+c"], "action": "client.tab.new", "source": "user"},
        {"keys": ["ctrl+a", "c"], "action": "greet", "source": "init.lua"},
        {"keys": ["ctrl+a", "x"], "action": "gone", "source": "init.lua"},
        {"keys": ["ctrl+shift+g"], "action": "greet", "args": {"n": 1}, "source": "init.lua"},
        {"keys": ["ctrl+shift+c"], "action": "pane.copy", "source": "puddlejumper"},
        {"keys": ["h"], "action": "pane.resize", "mode": "resize", "source": "init.lua"},
        {"keys": ["ctrl+shift+{"], "action": "pane.focus_previous", "source": "puddlejumper"}
      ],
      "modes": [{"name": "resize", "exclusive": true}, {"name": "loose", "exclusive": false}],
      "actions": [
        {"name": "greet", "kind": "lua", "title": "Greet", "category": "Test"},
        {"name": "com.superlogical.terminal.clear", "kind": "block", "title": "Clear Terminal"},
        {"name": "client.tab.new", "kind": "client", "title": "New Tab"}
      ],
      "diagnostics": [{"severity": "warning", "source": "init.lua", "message": "unknown action gone"}],
      "config": {"hash": "abc", "path": "/home/u/.config/rex/init.lua"}
    })").object();
}

}  // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // Spellings.
    check(spell(Qt::Key_T, Ctrl | Qt::ShiftModifier) == QStringList{"ctrl+shift+t"},
          QStringLiteral("Ctrl+Shift+T: %1").arg(spell(Qt::Key_T, Ctrl | Qt::ShiftModifier).join(',')));
    check(spell(Qt::Key_BraceLeft, Ctrl | Qt::ShiftModifier)
              == QStringList({"ctrl+shift+{", "ctrl+shift+["}),
          QStringLiteral("Ctrl+Shift+[ has both spellings"));
    check(spell(Qt::Key_Backtab, Ctrl) == QStringList{"ctrl+shift+tab"}, QStringLiteral("Backtab"));
    check(spell(Qt::Key_PageDown, Qt::ShiftModifier) == QStringList{"shift+pagedown"}, QStringLiteral("PgDown"));
    check(spell(Qt::Key_Escape, {}) == QStringList{"esc"}, QStringLiteral("Esc"));
    check(spell(Qt::Key_F5, Qt::AltModifier | Cmd) == QStringList{"alt+cmd+f5"}, QStringLiteral("Alt+Meta+F5"));
    check(spell(Qt::Key_Return, Ctrl | Qt::AltModifier) == QStringList{"ctrl+alt+enter"}, QStringLiteral("Enter"));
    check(spell(Qt::Key_Shift, Qt::ShiftModifier).isEmpty(), QStringLiteral("a lone modifier has no spelling"));
    check(RexKeymap::spell(QKeySequence(QStringLiteral("Ctrl+=")).operator[](0)) == kShortcutMod + QStringLiteral("+="),
          QStringLiteral("a shortcut's spelling"));
    check(RexKeymap::spell(QKeySequence(QStringLiteral("Ctrl+Shift+PgUp")).operator[](0)) == kShortcutMod + QStringLiteral("+shift+pageup"),
          QStringLiteral("Ctrl+Shift+PgUp"));
    check(RexKeymap::display({"ctrl+shift+t", "g"}) == QLatin1String("Ctrl+Shift+T G"), QStringLiteral("display"));
    check(RexKeymap::display({"ctrl++"}) == QLatin1String("Ctrl++"), QStringLiteral("display of +"));

    // Parsing.
    RexKeymap km;
    check(!km.isLoaded(), QStringLiteral("not loaded at first"));
    km.parse(keymapJson());
    check(km.isLoaded() && km.hash() == QLatin1String("abc"), QStringLiteral("hash"));
    check(km.actions().size() == 2 && km.action(QStringLiteral("greet")) && !km.action(QStringLiteral("client.tab.new")),
          QStringLiteral("the server's actions, not the client's"));
    check(km.diagnostics() == QStringList{"init.lua: unknown action gone"}, QStringLiteral("diagnostics"));

    // Lookups: the first match wins.
    const auto all = [](const QString &) { return true; };
    const RexKeymap::Binding *b = nullptr;
    check(km.lookup({}, {{"ctrl+shift+c"}}, all, &b) == RexKeymap::Result::Run && b->action == QLatin1String("client.tab.new"),
          QStringLiteral("an override wins"));
    const auto noTabs = [](const QString &a) { return a != QLatin1String("client.tab.new"); };
    check(km.lookup({}, {{"ctrl+shift+c"}}, noTabs, &b) == RexKeymap::Result::Run && b->action == QLatin1String("pane.copy"),
          QStringLiteral("an unavailable action is passed over"));
    check(km.lookup({}, {{"ctrl+a"}}, all) == RexKeymap::Result::Prefix, QStringLiteral("a sequence's start"));
    check(km.lookup({}, {{"ctrl+a"}, {"c"}}, all, &b) == RexKeymap::Result::Run && b->action == QLatin1String("greet"),
          QStringLiteral("a sequence"));
    check(km.lookup({}, {{"ctrl+a"}, {"q"}}, all) == RexKeymap::Result::Unbound, QStringLiteral("a broken sequence"));
    check(km.lookup({}, {{"ctrl+shift+g"}}, all, &b) == RexKeymap::Result::Run && b->args.value("n").toInt() == 1,
          QStringLiteral("arguments"));
    check(km.lookup({}, {{"ctrl+shift+[", "ctrl+shift+{"}}, all, &b) == RexKeymap::Result::Run,
          QStringLiteral("either spelling"));
    check(km.lookup({}, {{"h"}}, all) == RexKeymap::Result::Unbound, QStringLiteral("a mode's key, outside it"));
    check(km.lookup(QStringLiteral("resize"), {{"h"}}, all) == RexKeymap::Result::Run, QStringLiteral("in its mode"));
    check(km.lookup(QStringLiteral("resize"), {{"q"}}, all) == RexKeymap::Result::Swallow,
          QStringLiteral("an exclusive mode swallows"));
    check(km.lookup(QStringLiteral("loose"), {{"q"}}, all) == RexKeymap::Result::Unbound,
          QStringLiteral("another mode lets keys through"));
    check(km.keysFor(QStringLiteral("greet")) == QStringList({"ctrl+a", "c"}), QStringLiteral("keysFor"));
    check(km.keysFor(QStringLiteral("pane.copy")) == QStringList{"ctrl+shift+c"}, QStringLiteral("keysFor, shadowed"));

    // Key events as client.key sends them (libghostty's names).
    const auto ev = [](int key, Qt::KeyboardModifiers mods, const QString &text) {
        return RexKeymap::keyEvent(key, mods, text);
    };
    const auto is = [](const QJsonObject &e, const char *key, const char *mods, int cp, const char *text) {
        return e.value("key").toString() == QLatin1String(key) && e.value("mods").toString() == QLatin1String(mods)
               && e.value("unshifted_codepoint").toInt() == cp && e.value("text").toString() == QString::fromUtf8(text)
               && e.value("action").toString() == QLatin1String("press");
    };
    check(is(ev(Qt::Key_N, Ctrl, QStringLiteral("\x0e")), "key_n", "ctrl", 'n', ""), QStringLiteral("ctrl+n's event"));
    check(is(ev(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")), "key_a", "shift", 'a', "A")
              && ev(Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A")).value("consumed_mods").toString() == "shift",
          QStringLiteral("shift+a's event"));
    check(is(ev(Qt::Key_BraceLeft, Qt::ShiftModifier | Ctrl, QString()), "bracket_left", "shift+ctrl", '[', ""),
          QStringLiteral("a shifted symbol's event"));
    check(is(ev(Qt::Key_Exclam, Qt::ShiftModifier, QStringLiteral("!")), "digit_1", "shift", '1', "!"),
          QStringLiteral("shift+1's event"));
    check(is(ev(Qt::Key_Tab, Ctrl, QStringLiteral("\t")), "tab", "ctrl", 0, ""), QStringLiteral("ctrl+tab's event"));
    check(is(ev(Qt::Key_Backtab, Qt::ShiftModifier, QString()), "tab", "shift", 0, ""), QStringLiteral("backtab's event"));
    check(is(ev(Qt::Key_PageUp, Qt::NoModifier, QString()), "page_up", "", 0, ""), QStringLiteral("pageup's event"));
    check(is(ev(Qt::Key_Left, Cmd, QString()), "arrow_left", "super", 0, ""), QStringLiteral("cmd+left's event"));
    check(is(ev(Qt::Key_F5, Qt::NoModifier, QString()), "f5", "", 0, ""), QStringLiteral("f5's event"));
    check(is(ev(Qt::Key_Escape, Qt::NoModifier, QStringLiteral("\x1b")), "escape", "", 0, ""), QStringLiteral("esc's event"));
    check(is(ev(Qt::Key_5, Qt::KeypadModifier, QStringLiteral("5")), "numpad_5", "", '5', "5"), QStringLiteral("keypad 5"));
    check(is(ev(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")), "space", "", ' ', " "), QStringLiteral("space's event"));
    {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_N, Ctrl, QStringLiteral("\x0e"), true);
        check(RexKeymap::keyEvent(&e).value("action").toString() == QLatin1String("repeat"), QStringLiteral("a repeat"));
    }

    // A script's key handler: bound to client.key.forward, a sequence too.
    RexKeymap scripted;
    scripted.parse(QJsonDocument::fromJson(R"({"bindings": [
        {"keys": ["ctrl+n"], "action": "client.key.forward", "source": "keys.lua:1"},
        {"keys": ["ctrl+b", "x"], "action": "client.key.forward", "source": "keys.lua:2"}]})").object());
    check(scripted.lookup({}, {{"ctrl+n"}}, all, &b) == RexKeymap::Result::Run && b->action == QLatin1String("client.key.forward"),
          QStringLiteral("a forwarded key"));
    check(scripted.lookup({}, {{"ctrl+b"}, {"x"}}, all, &b) == RexKeymap::Result::Run && b->keys.mid(0, 1) == QStringList{"ctrl+b"},
          QStringLiteral("a forwarded sequence, and its prefix"));

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
