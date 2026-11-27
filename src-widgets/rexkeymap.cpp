/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexkeymap.h"

#include <QHash>
#include <QJsonArray>
#include <QKeyEvent>

namespace {

QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

QStringList strings(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &s : v.toArray()) {
        if (s.isString())
            out << s.toString();
    }
    return out;
}

// The key's own name, as rexia spells it; empty for a lone modifier.
QString keyName(int key)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("enter");
    case Qt::Key_Escape:
        return QStringLiteral("esc");
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return QStringLiteral("tab");
    case Qt::Key_Backspace:
        return QStringLiteral("backspace");
    case Qt::Key_Delete:
        return QStringLiteral("delete");
    case Qt::Key_Insert:
        return QStringLiteral("insert");
    case Qt::Key_Home:
        return QStringLiteral("home");
    case Qt::Key_End:
        return QStringLiteral("end");
    case Qt::Key_PageUp:
        return QStringLiteral("pageup");
    case Qt::Key_PageDown:
        return QStringLiteral("pagedown");
    case Qt::Key_Left:
        return QStringLiteral("left");
    case Qt::Key_Right:
        return QStringLiteral("right");
    case Qt::Key_Up:
        return QStringLiteral("up");
    case Qt::Key_Down:
        return QStringLiteral("down");
    case Qt::Key_Space:
        return QStringLiteral("space");
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_unknown:
        return {};
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return QStringLiteral("f%1").arg(key - Qt::Key_F1 + 1);
    if (key > 0x20 && key < 0x7f)
        return QString(QChar(key)).toLower();
    if (key >= 0xa0 && key <= 0xff)
        return QString(QChar(key)).toLower();
    return {};
}

// A shifted symbol's key on the US layout.
QChar unshifted(QChar c)
{
    static const QHash<QChar, QChar> map = [] {
        QHash<QChar, QChar> m;
        const QString from = QStringLiteral("~!@#$%^&*()_+{}|:\"<>?");
        const QString to = QStringLiteral("`1234567890-=[]\\;',./");
        for (int i = 0; i < from.size(); ++i)
            m.insert(from.at(i), to.at(i));
        return m;
    }();
    return map.value(c);
}

QString withMods(bool ctrl, bool alt, bool shift, bool cmd, const QString &key)
{
    QString out;
    if (ctrl)
        out += QLatin1String("ctrl+");
    if (alt)
        out += QLatin1String("alt+");
    if (shift)
        out += QLatin1String("shift+");
    if (cmd)
        out += QLatin1String("cmd+");
    return out + key;
}

}  // namespace

void RexKeymap::parse(const QJsonObject &reply)
{
    m_loaded = true;
    m_bindings.clear();
    m_actions.clear();
    m_diagnostics.clear();
    m_modes.clear();
    const QJsonObject config = reply.value(QLatin1String("config")).toObject();
    m_hash = text(config, "hash");
    m_configPath = text(config, "path");
    for (const QJsonValue &v : reply.value(QLatin1String("bindings")).toArray()) {
        const QJsonObject b = v.toObject();
        Binding binding{strings(b.value(QLatin1String("keys"))), text(b, "action"),
                        b.value(QLatin1String("args")).toObject(), text(b, "mode"), text(b, "source")};
        if (!binding.keys.isEmpty() && !binding.action.isEmpty())
            m_bindings << binding;
    }
    for (const QJsonValue &v : reply.value(QLatin1String("modes")).toArray()) {
        const QJsonObject m = v.toObject();
        if (!text(m, "name").isEmpty())
            m_modes.insert(text(m, "name"), m.value(QLatin1String("exclusive")).toBool());
    }
    for (const QJsonValue &v : reply.value(QLatin1String("actions")).toArray()) {
        const QJsonObject a = v.toObject();
        Action action{text(a, "name"), text(a, "kind"), text(a, "title"), text(a, "category"),
                      text(a, "description"), text(a, "keywords")};
        if (action.name.isEmpty() || action.kind == QLatin1String("client"))
            continue;
        action.titled = !action.title.isEmpty();
        if (!action.titled)
            action.title = action.name;
        m_actions << action;
    }
    for (const QJsonValue &v : reply.value(QLatin1String("diagnostics")).toArray()) {
        const QJsonObject d = v.toObject();
        const QString message = text(d, "message");
        if (message.isEmpty())
            continue;
        const QString source = text(d, "source");
        m_diagnostics << (source.isEmpty() ? message : source + QStringLiteral(": ") + message);
    }
}

QList<RexKeymap::Action> RexKeymap::peopleActions() const
{
    QList<Action> out;
    for (const Action &a : m_actions) {
        if (a.titled || a.kind != QLatin1String("block"))
            out << a;
    }
    return out;
}

const RexKeymap::Action *RexKeymap::action(const QString &name) const
{
    for (const Action &a : m_actions) {
        if (a.name == name)
            return &a;
    }
    return nullptr;
}

bool RexKeymap::hasMode(const QString &mode) const
{
    return m_modes.contains(mode);
}

RexKeymap::Result RexKeymap::lookup(const QString &mode, const QList<QStringList> &typed,
                                    const std::function<bool(const QString &)> &available,
                                    const Binding **found) const
{
    if (typed.isEmpty())
        return Result::Unbound;
    bool prefix = false;
    for (const Binding &b : m_bindings) {
        if (b.mode != mode || b.keys.size() < typed.size())
            continue;
        bool matches = true;
        for (qsizetype i = 0; i < typed.size() && matches; ++i)
            matches = typed.at(i).contains(b.keys.at(i));
        if (!matches)
            continue;
        if (b.keys.size() == typed.size()) {
            if (!available || available(b.action)) {
                if (found)
                    *found = &b;
                return Result::Run;
            }
        } else {
            prefix = true;
        }
    }
    if (prefix)
        return Result::Prefix;
    if (!mode.isEmpty()) {
        if (m_modes.value(mode))
            return Result::Swallow;
    }
    return Result::Unbound;
}

QStringList RexKeymap::keysFor(const QString &action) const
{
    for (const Binding &b : m_bindings) {
        if (b.action == action && b.mode.isEmpty() && b.args.isEmpty())
            return b.keys;
    }
    return {};
}

QStringList RexKeymap::spellings(const QKeyEvent *event)
{
    return spellings(event->key(), event->modifiers());
}

QStringList RexKeymap::spellings(int key, Qt::KeyboardModifiers modifiers)
{
#ifdef Q_OS_MACOS
    const bool ctrl = modifiers & Qt::MetaModifier;
    const bool cmd = modifiers & Qt::ControlModifier;
#else
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool cmd = modifiers & Qt::MetaModifier;
#endif
    const bool alt = modifiers & Qt::AltModifier;
    bool shift = modifiers & Qt::ShiftModifier;
    if (key == Qt::Key_Backtab)
        shift = true;
    const QString name = keyName(key);
    if (name.isEmpty())
        return {};
    QStringList out{withMods(ctrl, alt, shift, cmd, name)};
    // A shifted symbol, also as its key: Shift+{ is Shift+[.
    if (name.size() == 1) {
        const QChar base = unshifted(name.at(0));
        if (!base.isNull())
            out << withMods(ctrl, alt, true, cmd, QString(base));
    }
    out.removeDuplicates();
    return out;
}

QJsonObject RexKeymap::keyEvent(const QKeyEvent *event)
{
    return keyEvent(event->key(), event->modifiers(), event->text(), event->isAutoRepeat());
}

QJsonObject RexKeymap::keyEvent(int key, Qt::KeyboardModifiers modifiers, const QString &text, bool repeat)
{
#ifdef Q_OS_MACOS
    const bool ctrl = modifiers & Qt::MetaModifier;
    const bool super = modifiers & Qt::ControlModifier;
#else
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool super = modifiers & Qt::MetaModifier;
#endif
    const bool alt = modifiers & Qt::AltModifier;
    const bool shift = (modifiers & Qt::ShiftModifier) || key == Qt::Key_Backtab;
    const bool keypad = modifiers & Qt::KeypadModifier;

    // libghostty's key name (W3C codes in snake case), and the character the
    // key gives unshifted (US layout).
    QString name = QStringLiteral("unidentified");
    QChar base;
    static const QHash<QChar, QString> symbols{
        {QLatin1Char('`'), QStringLiteral("backquote")},     {QLatin1Char('-'), QStringLiteral("minus")},
        {QLatin1Char('='), QStringLiteral("equal")},         {QLatin1Char('['), QStringLiteral("bracket_left")},
        {QLatin1Char(']'), QStringLiteral("bracket_right")}, {QLatin1Char('\\'), QStringLiteral("backslash")},
        {QLatin1Char(';'), QStringLiteral("semicolon")},     {QLatin1Char('\''), QStringLiteral("quote")},
        {QLatin1Char(','), QStringLiteral("comma")},         {QLatin1Char('.'), QStringLiteral("period")},
        {QLatin1Char('/'), QStringLiteral("slash")},
    };
    static const QHash<QChar, QString> keypadSymbols{
        {QLatin1Char('+'), QStringLiteral("numpad_add")},      {QLatin1Char('-'), QStringLiteral("numpad_subtract")},
        {QLatin1Char('*'), QStringLiteral("numpad_multiply")}, {QLatin1Char('/'), QStringLiteral("numpad_divide")},
        {QLatin1Char('.'), QStringLiteral("numpad_decimal")},  {QLatin1Char('='), QStringLiteral("numpad_equal")},
        {QLatin1Char(','), QStringLiteral("numpad_comma")},
    };
    switch (key) {
    case Qt::Key_Return: name = QStringLiteral("enter"); break;
    case Qt::Key_Enter: name = keypad ? QStringLiteral("numpad_enter") : QStringLiteral("enter"); break;
    case Qt::Key_Escape: name = QStringLiteral("escape"); break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: name = QStringLiteral("tab"); break;
    case Qt::Key_Backspace: name = QStringLiteral("backspace"); break;
    case Qt::Key_Delete: name = QStringLiteral("delete"); break;
    case Qt::Key_Insert: name = QStringLiteral("insert"); break;
    case Qt::Key_Home: name = QStringLiteral("home"); break;
    case Qt::Key_End: name = QStringLiteral("end"); break;
    case Qt::Key_PageUp: name = QStringLiteral("page_up"); break;
    case Qt::Key_PageDown: name = QStringLiteral("page_down"); break;
    case Qt::Key_Left: name = QStringLiteral("arrow_left"); break;
    case Qt::Key_Right: name = QStringLiteral("arrow_right"); break;
    case Qt::Key_Up: name = QStringLiteral("arrow_up"); break;
    case Qt::Key_Down: name = QStringLiteral("arrow_down"); break;
    case Qt::Key_Space:
        name = QStringLiteral("space");
        base = QLatin1Char(' ');
        break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F25) {
            name = QStringLiteral("f%1").arg(key - Qt::Key_F1 + 1);
        } else if (key > 0x20 && key < 0x7f) {
            QChar c = QChar(key).toLower();
            if (const QChar u = unshifted(c); !u.isNull())
                c = u;
            base = c;
            if (keypad && c.isDigit())
                name = QStringLiteral("numpad_") + c;
            else if (keypad && keypadSymbols.contains(QChar(key)))
                name = keypadSymbols.value(QChar(key));
            else if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                name = QStringLiteral("key_") + c;
            else if (c.isDigit())
                name = QStringLiteral("digit_") + c;
            else
                name = symbols.value(c, name);
        }
    }

    QStringList mods;
    if (shift)
        mods << QStringLiteral("shift");
    if (ctrl)
        mods << QStringLiteral("ctrl");
    if (alt)
        mods << QStringLiteral("alt");
    if (super)
        mods << QStringLiteral("super");
    // The text the key gives, without control characters (libghostty makes
    // ctrl+n's from the key); shift is spent on it when it changed it.
    QString typed;
    for (const QChar c : text)
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f)
            typed += c;
    const bool shiftSpent = shift && !typed.isEmpty() && !base.isNull() && typed != QString(base);
    return QJsonObject{
        {QStringLiteral("key"), name},
        {QStringLiteral("mods"), mods.join(QLatin1Char('+'))},
        {QStringLiteral("consumed_mods"), shiftSpent ? QStringLiteral("shift") : QString()},
        {QStringLiteral("text"), typed},
        {QStringLiteral("unshifted_codepoint"), base.isNull() ? 0 : int(base.unicode())},
        {QStringLiteral("composing"), false},
        {QStringLiteral("action"), repeat ? QStringLiteral("repeat") : QStringLiteral("press")},
    };
}

QString RexKeymap::spell(QKeyCombination combination)
{
    return spellings(combination.key(), combination.keyboardModifiers()).value(0);
}

QString RexKeymap::display(const QStringList &keys)
{
    QStringList strokes;
    for (const QString &k : keys) {
        QStringList parts = k.split(QLatin1Char('+'));
        // "ctrl++": the key is "+".
        if (k.endsWith(QLatin1String("++"))) {
            parts.removeLast();
            parts.last() = QStringLiteral("+");
        }
        for (QString &p : parts) {
            if (p == QLatin1String("ctrl"))
                p = QStringLiteral("Ctrl");
            else if (p == QLatin1String("alt"))
                p = QStringLiteral("Alt");
            else if (p == QLatin1String("shift"))
                p = QStringLiteral("Shift");
            else if (p == QLatin1String("cmd"))
#ifdef Q_OS_MACOS
                p = QStringLiteral("Cmd");
#else
                p = QStringLiteral("Meta");
#endif
            else if (p == QLatin1String("pageup"))
                p = QStringLiteral("PgUp");
            else if (p == QLatin1String("pagedown"))
                p = QStringLiteral("PgDown");
            else if (p == QLatin1String("esc"))
                p = QStringLiteral("Esc");
            else if (!p.isEmpty())
                p[0] = p.at(0).toUpper();
        }
        strokes << parts.join(QLatin1Char('+'));
    }
    return strokes.join(QLatin1Char(' '));
}
