/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */
#include "via.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

Q_LOGGING_CATEGORY(lcVia, "puddlejumper.via")

namespace {

const QByteArray kOpener = "\x1b]6522;";
constexpr qsizetype kMaxMessage = 1 << 20;  // what attach asks for
constexpr int kLease = 120;                  // seconds
const QString kSchema = QStringLiteral("puddle.2");

// key=value pairs; empty if a value is outside the protocol's alphabet.
QHash<QString, QString> keys(const QByteArray &body, bool *ok)
{
    QHash<QString, QString> kv;
    *ok = true;
    for (const QByteArray &pair : body.split(':')) {
        const qsizetype eq = pair.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray v = pair.mid(eq + 1);
        for (char c : v) {
            const bool fine = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || QByteArrayView("_.,+/=-").contains(c);
            if (!fine) {
                *ok = false;
                return {};
            }
        }
        kv.insert(QString::fromLatin1(pair.left(eq)), QString::fromLatin1(v));
    }
    return kv;
}

QString randomId()
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    QString s;
    for (int i = 0; i < 16; ++i)
        s += QLatin1Char(letters[QRandomGenerator::global()->bounded(62)]);
    return s;
}

QByteArray wrap(const QString &kv) { return kOpener + kv.toLatin1() + "\x1b\\"; }

} // namespace

// ---- ViaScanner ---------------------------------------------------------------

QList<QByteArray> ViaScanner::scan(const QByteArray &data)
{
    QList<QByteArray> out;
    QByteArray buf = m_carry + data;
    m_carry.clear();
    qsizetype at = 0;
    while (at < buf.size()) {
        const qsizetype start = buf.indexOf(kOpener, at);
        if (start < 0) {
            // Keep a tail that could be the start of an opener.
            for (qsizetype keep = qMin<qsizetype>(kOpener.size() - 1, buf.size() - at); keep > 0; --keep) {
                if (kOpener.startsWith(QByteArrayView(buf).last(keep))) {
                    m_carry = buf.right(keep);
                    break;
                }
            }
            break;
        }
        const qsizetype body = start + kOpener.size();
        qsizetype end = -1, next = -1;
        for (qsizetype i = body; i < buf.size(); ++i) {
            const char c = buf[i];
            if (c == '\x07') {
                end = i, next = i + 1;
                break;
            }
            if (c == '\x1b') {
                if (i + 1 == buf.size())
                    break;  // the terminator may be cut short
                if (buf[i + 1] == '\\') {
                    end = i, next = i + 2;
                    break;
                }
                end = -2, next = i;  // cancelled by another sequence
                break;
            }
        }
        if (end == -2) {
            at = next;
            continue;
        }
        if (end < 0) {
            if (buf.size() - start <= kMaxMessage + 64)
                m_carry = buf.mid(start);
            break;
        }
        out.append(buf.mid(body, end - body));
        at = next;
    }
    return out;
}

// ---- ViaClient ----------------------------------------------------------------

ViaClient::ViaClient(QObject *parent)
    : QObject(parent)
{
    m_renew = new QTimer(this);
    m_renew->setInterval(kLease * 1000 / 2);
    connect(m_renew, &QTimer::timeout, this, [this]() {
        if (m_attached)
            attach(false);
    });
}

void ViaClient::feed(const QByteArray &output)
{
    if (!output.contains("6522") && !output.contains('\x1b'))
        return;
    for (const QByteArray &body : m_scanner.scan(output))
        message(body);
}

void ViaClient::message(const QByteArray &body)
{
    bool ok = false;
    const QHash<QString, QString> kv = keys(body, &ok);
    if (!ok)
        return;
    const QString op = kv.value(QStringLiteral("op"));
    if (op == QLatin1String("hello")) {
        hello(kv);
        return;
    }
    if (op == QLatin1String("bye")) {
        if (kv.value(QStringLiteral("inst")) == m_inst)
            end();
        return;
    }
    if (!m_attached)
        return;
    const QString win = kv.value(QStringLiteral("win"));
    bool seqOk = false;
    const quint64 seq = kv.value(QStringLiteral("seq")).toULongLong(&seqOk);
    if (win.isEmpty() || !seqOk)
        return;
    auto it = m_windows.find(win);
    if (op == QLatin1String("close")) {
        if (it != m_windows.end() && seq > it->second.seq) {
            m_windows.erase(it);
            m_order.removeAll(win);
            emit windowsChanged();
        }
        return;
    }
    if (op != QLatin1String("frame"))
        return;  // state messages: version 1 of puddle's server sends whole frames
    const int part = kv.value(QStringLiteral("part"), QStringLiteral("1")).toInt();
    const int parts = kv.value(QStringLiteral("parts"), QStringLiteral("1")).toInt();
    if (part == 1) {
        m_partWin = win, m_partSeq = seq, m_partNext = 1;
        m_partData.clear();
    }
    if (win != m_partWin || seq != m_partSeq || part != m_partNext || parts < part) {
        m_partWin.clear();
        return;
    }
    m_partData += kv.value(QStringLiteral("data")).toLatin1();
    ++m_partNext;
    if (part < parts)
        return;
    m_partWin.clear();
    if (it != m_windows.end() && seq <= it->second.seq)
        return;
    const QByteArray json = QByteArray::fromBase64(m_partData);
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (!doc.isObject()) {
        qCInfo(lcVia) << "bad frame for" << win << error.errorString();
        return;
    }
    Window &w = m_windows[win];
    w.id = win, w.seq = seq, w.frame = doc.object();
    m_order.removeAll(win);
    m_order.append(win);
    emit windowsChanged();
}

void ViaClient::hello(const QHash<QString, QString> &kv)
{
    if (kv.value(QStringLiteral("v")) != QLatin1String("1"))
        return;
    const QString inst = kv.value(QStringLiteral("inst"));
    if (!kv.value(QStringLiteral("schema")).split(QLatin1Char(',')).contains(kSchema) || inst.size() < 8)
        return;
    if (inst == m_declined)
        return;
    if (m_attached && inst != m_inst)
        end();  // another program
    if (m_attached || m_checking)
        return;
    const int pid = kv.value(QStringLiteral("pid")).toInt();
    if (pid <= 0 || !m_check)
        return;
    m_checking = true;
    QPointer<ViaClient> self(this);
    const QString app = kv.value(QStringLiteral("app"));
    const int max = qBound(4096, kv.value(QStringLiteral("max")).toInt(), int(kMaxMessage));
    m_check(pid, [self, inst, pid, app, max](bool foreground) {
        if (!self)
            return;
        self->m_checking = false;
        if (!foreground) {
            qCInfo(lcVia) << "hello from pid" << pid << "which isn't the foreground process; ignored";
            return;
        }
        self->m_inst = inst, self->m_pid = pid, self->m_app = app, self->m_max = max;
        if (self->m_client.isEmpty())
            self->m_client = randomId();
        self->m_attached = true;
        self->attach(true);
        self->m_renew->start();
    });
}

void ViaClient::attach(bool full)
{
    emit send(wrap(QStringLiteral("op=attach:v=1:client=%1:schema=%2:max=%3:lease=%4%5")
                          .arg(m_client, kSchema)
                          .arg(kMaxMessage)
                          .arg(kLease)
                          .arg(full ? QStringLiteral(":full=1") : QString())),
              false);
}

void ViaClient::foregroundChanged(int pid)
{
    if (m_attached && pid != m_pid)
        end();
}

void ViaClient::resynced()
{
    if (m_attached)
        attach(true);
}

void ViaClient::end()
{
    const bool was = m_attached;
    m_attached = false;
    m_renew->stop();
    m_windows.clear();
    m_order.clear();
    m_inst.clear();
    if (was)
        emit ended();
}

void ViaClient::detach()
{
    if (!m_attached)
        return;
    emit send(wrap(QStringLiteral("op=detach:client=%1").arg(m_client)), false);
    m_declined = m_inst;
    end();
}

QList<const ViaClient::Window *> ViaClient::windows() const
{
    QList<const Window *> out;
    for (const QString &id : m_order) {
        auto it = m_windows.find(id);
        if (it != m_windows.end())
            out.append(&it->second);
    }
    return out;
}

void ViaClient::sendEvent(const QString &win, const QJsonObject &data)
{
    if (!m_attached)
        return;
    ++m_events;
    quint64 ack = 0;
    auto it = m_windows.find(win);
    if (it != m_windows.end())
        ack = it->second.seq;
    QByteArray b64 = QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64(QByteArray::OmitTrailingEquals);
    const QString head = QStringLiteral("op=event:client=%1:win=%2:ack=%3:n=%4").arg(m_client, win).arg(ack).arg(m_events);
    const qsizetype room = m_max - head.size() - 64;
    const qsizetype parts = qMax<qsizetype>(1, (b64.size() + room - 1) / room);
    QByteArray out;
    for (qsizetype p = 0; p < parts; ++p) {
        const QString piece = QString::fromLatin1(b64.mid(p * room, room));
        out += wrap(parts == 1 ? head + QStringLiteral(":data=") + piece
                                  : head + QStringLiteral(":part=%1:parts=%2:data=%3").arg(p + 1).arg(parts).arg(piece));
    }
    emit send(out, true);
}

// ---- ViaView ------------------------------------------------------------------

ViaView::ViaView(ViaClient *client, QWidget *parent)
    : QWidget(parent)
    , m_client(client)
{
    setAutoFillBackground(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 0);
    bar->addStretch();
    auto *back = new QToolButton(this);
    back->setObjectName(QStringLiteral("viaCells"));
    back->setText(tr("Text view"));
    back->setToolTip(tr("Show %1 as text again").arg(client->app().isEmpty() ? tr("the program") : client->app()));
    back->setAutoRaise(true);
    connect(back, &QToolButton::clicked, this, &ViaView::cellsRequested);
    bar->addWidget(back);
    layout->addLayout(bar);
    connect(client, &ViaClient::windowsChanged, this, &ViaView::rebuild);
    rebuild();
}

void ViaView::setColours(const QColor &background, const QColor &foreground, const QColor &accent)
{
    m_bg = background, m_fg = foreground, m_accent = accent;
    restyle();
    m_built.clear();
    rebuild();
}

void ViaView::restyle()
{
    QPalette p = palette();
    if (m_bg.isValid()) {
        p.setColor(QPalette::Window, m_bg);
        p.setColor(QPalette::Base, m_bg.lightnessF() < 0.5 ? m_bg.lighter(130) : m_bg.darker(108));
        p.setColor(QPalette::Button, m_bg.lightnessF() < 0.5 ? m_bg.lighter(150) : m_bg.darker(115));
    }
    if (m_fg.isValid()) {
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            p.setColor(role, m_fg);
    }
    if (m_accent.isValid())
        p.setColor(QPalette::Highlight, m_accent);
    setPalette(p);
}

void ViaView::rebuild()
{
    if (!m_client)
        return;
    const QList<const ViaClient::Window *> wins = m_client->windows();
    QString key;
    for (const auto *w : wins)
        key += w->id + QLatin1Char('\n') + QString::fromUtf8(QJsonDocument(w->frame.value(QLatin1String("root")).toObject())
                                                                 .toJson(QJsonDocument::Compact));
    // State alone changed (text typed, a box ticked): update in place, so
    // the widget being edited keeps its focus and cursor.
    if (key == m_built && m_body) {
        for (const auto *w : wins) {
            const QJsonObject state = w->frame.value(QLatin1String("state")).toObject();
            for (auto it = state.begin(); it != state.end(); ++it) {
                const QString name = w->id + QLatin1Char('/') + it.key();
                const QJsonObject s = it.value().toObject();
                if (auto *edit = m_body->findChild<QLineEdit *>(name); edit && !edit->hasFocus())
                    edit->setText(s.value(QLatin1String("text")).toString());
                if (auto *box = m_body->findChild<QCheckBox *>(name))
                    box->setChecked(s.value(QLatin1String("on")).toBool());
            }
        }
        return;
    }
    // Remember what had focus, to give it back.
    QString focused;
    if (QWidget *f = focusWidget(); f && isAncestorOf(f))
        focused = f->objectName();
    delete m_body;
    m_body = new QWidget(this);
    auto *stack = new QVBoxLayout(m_body);
    stack->setContentsMargins(16, 8, 16, 16);
    // The bottom window fills the view; modal windows go above it, framed.
    for (const auto *w : wins) {
        const QJsonObject state = w->frame.value(QLatin1String("state")).toObject();
        QWidget *content = build(w->id, w->frame.value(QLatin1String("root")).toObject(), state);
        if (w->frame.value(QLatin1String("modal")).toBool() && w != wins.first()) {
            auto *frame = new QFrame(m_body);
            frame->setFrameShape(QFrame::StyledPanel);
            auto *fl = new QVBoxLayout(frame);
            auto *title = new QLabel(w->frame.value(QLatin1String("title")).toString(), frame);
            QFont bold = title->font();
            bold.setBold(true);
            title->setFont(bold);
            fl->addWidget(title);
            fl->addWidget(content);
            stack->addWidget(frame, 0, Qt::AlignHCenter);
        } else {
            stack->addWidget(content, 1);
        }
    }
    static_cast<QVBoxLayout *>(layout())->addWidget(m_body, 1);
    m_built = key;
    if (!focused.isEmpty()) {
        if (QWidget *f = m_body->findChild<QWidget *>(focused))
            f->setFocus();
    }
}

QWidget *ViaView::build(const QString &win, const QJsonObject &node, const QJsonObject &state)
{
    const QString type = node.value(QLatin1String("type")).toString();
    const QString id = node.value(QLatin1String("id")).toString();
    const QString name = win + QLatin1Char('/') + id;
    const QJsonObject st = state.value(id).toObject();
    QPointer<ViaClient> client = m_client;
    auto send = [client, win, id](const QString &act, const QJsonObject &more = {}) {
        if (!client)
            return;
        QJsonObject data = more;
        data.insert(QStringLiteral("node"), id);
        data.insert(QStringLiteral("act"), act);
        client->sendEvent(win, data);
    };
    if (type == QLatin1String("column") || type == QLatin1String("row") || type == QLatin1String("box")) {
        auto *w = new QWidget;
        QBoxLayout *l = type == QLatin1String("row") ? static_cast<QBoxLayout *>(new QHBoxLayout(w))
                                                     : static_cast<QBoxLayout *>(new QVBoxLayout(w));
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(node.value(QLatin1String("gap")).toInt(1) * 4 + 4);
        for (const QJsonValue &child : node.value(QLatin1String("children")).toArray())
            l->addWidget(build(win, child.toObject(), state));
        if (type != QLatin1String("row"))
            l->addStretch();
        return w;
    }
    if (type == QLatin1String("label")) {
        auto *l = new QLabel(node.value(QLatin1String("text")).toString());
        l->setWordWrap(!node.value(QLatin1String("nowrap")).toBool());
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        const QString style = node.value(QLatin1String("style")).toString();
        if (style == QLatin1String("accent") && m_accent.isValid())
            l->setStyleSheet(QStringLiteral("color: %1;").arg(m_accent.name()));
        else if (style == QLatin1String("dim"))
            l->setEnabled(false);
        else if (style == QLatin1String("bold") || style == QLatin1String("title")) {
            QFont f = l->font();
            f.setBold(true);
            l->setFont(f);
        }
        return l;
    }
    if (type == QLatin1String("button")) {
        auto *b = new QPushButton(node.value(QLatin1String("label")).toString());
        b->setObjectName(name);
        b->setDefault(node.value(QLatin1String("primary")).toBool());
        QObject::connect(b, &QPushButton::clicked, b, [send]() { send(QStringLiteral("click")); });
        return b;
    }
    if (type == QLatin1String("text_input")) {
        auto *e = new QLineEdit;
        e->setObjectName(name);
        e->setPlaceholderText(node.value(QLatin1String("placeholder")).toString());
        e->setText(st.value(QLatin1String("text")).toString());
        QObject::connect(e, &QLineEdit::textEdited, e, [send](const QString &text) {
            send(QStringLiteral("text"), QJsonObject{{QStringLiteral("value"), text}});
        });
        QObject::connect(e, &QLineEdit::returnPressed, e, [send]() { send(QStringLiteral("submit")); });
        return e;
    }
    if (type == QLatin1String("checkbox")) {
        auto *c = new QCheckBox(node.value(QLatin1String("label")).toString());
        c->setObjectName(name);
        c->setChecked(st.value(QLatin1String("on")).toBool());
        // The program toggles it, and its next frame says how it stands.
        QObject::connect(c, &QCheckBox::clicked, c, [send]() { send(QStringLiteral("click")); });
        return c;
    }
    if (type == QLatin1String("spacer")) {
        auto *s = new QWidget;
        // Room along a row; a column ends in a stretch of its own.
        s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        return s;
    }
    // Not drawn natively yet.
    auto *l = new QLabel(QStringLiteral("[%1]").arg(type));
    l->setEnabled(false);
    return l;
}
