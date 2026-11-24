/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "tmuxcontrol.h"

#include "disclaim.h"

#include "terminaltab.h"   // PaneLayout

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

Q_LOGGING_CATEGORY(lcTmux, "puddlejumper.tmux")

namespace {

/* "%12" or "@3" or "$1" -> 12 / 3 / 1; -1 if malformed. */
int parseId(const QByteArray &token)
{
    if (token.size() < 2)
        return -1;
    bool ok = false;
    const int id = token.mid(1).toInt(&ok);
    return ok ? id : -1;
}

/* Fields of a notification line, split on single spaces. The last field
 * keeps any spaces it contains when `max` limits the split. */
QList<QByteArray> fields(const QByteArray &line, int max)
{
    QList<QByteArray> out;
    int start = 0;
    while (out.size() < max - 1) {
        const int space = line.indexOf(' ', start);
        if (space < 0)
            break;
        out << line.mid(start, space - start);
        start = space + 1;
    }
    out << line.mid(start);
    return out;
}

// ---- layout strings ----------------------------------------------------------

bool parseNumber(const char *&p, const char *end, int &out)
{
    const char *begin = p;
    int value = 0;
    while (p < end && *p >= '0' && *p <= '9')
        value = value * 10 + (*p++ - '0');
    out = value;
    return p != begin;
}

bool expect(const char *&p, const char *end, char c)
{
    if (p < end && *p == c) {
        ++p;
        return true;
    }
    return false;
}

/* cell := W 'x' H ',' X ',' Y ( ',' ID | '{' cell (',' cell)* '}' | '[' … ']' ) */
bool parseCell(const char *&p, const char *end, PaneLayout &out)
{
    int x = 0;
    int y = 0;
    if (!parseNumber(p, end, out.columns) || !expect(p, end, 'x')
        || !parseNumber(p, end, out.lines) || !expect(p, end, ',')
        || !parseNumber(p, end, x) || !expect(p, end, ',')
        || !parseNumber(p, end, y))
        return false;

    if (p < end && (*p == '{' || *p == '[')) {
        const char close = (*p == '{') ? '}' : ']';
        out.orientation = (*p == '{') ? Qt::Horizontal : Qt::Vertical;
        ++p;
        do {
            PaneLayout child;
            if (!parseCell(p, end, child))
                return false;
            out.children << child;
        } while (expect(p, end, ','));
        return expect(p, end, close);
    }
    return expect(p, end, ',') && parseNumber(p, end, out.paneId);
}

/*
 * tmux 3.8's JSON layout ({"V":2,"L":{…}}), which format variables such as
 * #{window_layout} now produce: "t" is "p" for a pane, "h" for side by side
 * or "v" for stacked; "w"/"h" its size in cells; "I" a pane's id ("%3"); "c"
 * the children.
 */
bool parseJsonCell(const QJsonObject &node, PaneLayout &out)
{
    out.columns = node.value(QLatin1String("w")).toInt();
    out.lines = node.value(QLatin1String("h")).toInt();
    const QString type = node.value(QLatin1String("t")).toString();
    if (type == QLatin1String("p")) {
        const QString id = node.value(QLatin1String("I")).toString();
        bool ok = false;
        out.paneId = id.mid(1).toInt(&ok);
        return ok;
    }
    if (type != QLatin1String("h") && type != QLatin1String("v"))
        return false;
    out.orientation = (type == QLatin1String("h")) ? Qt::Horizontal : Qt::Vertical;
    const QJsonArray children = node.value(QLatin1String("c")).toArray();
    for (const QJsonValue &c : children) {
        PaneLayout child;
        if (!parseJsonCell(c.toObject(), child))
            return false;
        out.children << child;
    }
    return !out.children.isEmpty();
}

} // namespace

TmuxControl::TmuxControl(QObject *parent)
    : QObject(parent)
{
}

TmuxControl::~TmuxControl()
{
    if (isRunning()) {
        // Leave the session running for next time: detach, don't kill.
        disconnect(m_process, nullptr, this, nullptr);
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(1000))
            m_process->kill();
    }
}

void TmuxControl::start(const QStringList &command)
{
    if (command.isEmpty() || m_process)
        return;

    m_process = new QProcess(this);
    // stderr carries tmux's own complaints ("no server running", ssh errors);
    // keep it separate so it can't corrupt the protocol, and report it on exit.
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &TmuxControl::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        qCInfo(lcTmux) << "process error" << error << m_process->errorString();
        if (error == QProcess::FailedToStart && !m_exited) {
            m_exited = true;
            emit exited(tr("Could not run %1: %2")
                            .arg(m_process->program(), m_process->errorString()));
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        qCInfo(lcTmux) << "exited: code" << code << status << "reason" << m_exitReason;
        if (m_exited)
            return;
        m_exited = true;
        QString reason = m_exitReason;
        if (reason.isEmpty()) {
            const QString err = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
            reason = !err.isEmpty() ? err
                     : code != 0    ? tr("tmux exited with status %1").arg(code)
                                    : QString();
        }
        // Commands still waiting will never be answered.
        while (!m_pending.isEmpty()) {
            if (Reply reply = m_pending.dequeue())
                reply(false, {});
        }
        emit exited(reason);
    });

    qCInfo(lcTmux) << "starting" << command;
    // It may start the tmux server, which mustn't count as part of the app.
    const QStringList run = disclaimedCommand(command);
    m_process->start(run.first(), run.mid(1));
}

bool TmuxControl::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning && !m_exited;
}

void TmuxControl::command(const QString &cmd, Reply reply)
{
    if (!isRunning()) {
        if (reply)
            reply(false, {});
        return;
    }
    m_pending.enqueue(std::move(reply));
    m_process->write(cmd.toUtf8() + '\n');
}

void TmuxControl::detach()
{
    qCInfo(lcTmux) << "detaching; running:" << isRunning();
    if (!isRunning())
        return;
    m_process->write("detach-client\n");
    m_process->closeWriteChannel();
}

QString TmuxControl::quote(const QString &arg)
{
    // Double quotes, in which tmux takes \ as an escape and expands $ (so both
    // are escaped, along with the quote itself).
    QString out;
    out.reserve(arg.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : arg) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\') || c == QLatin1Char('$'))
            out += QLatin1Char('\\');
        out += c;
    }
    out += QLatin1Char('"');
    return out;
}

QByteArray TmuxControl::unescape(const QByteArray &text)
{
    QByteArray out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const char c = text.at(i);
        if (c == '\\' && i + 3 < text.size()) {
            const char a = text.at(i + 1), b = text.at(i + 2), d = text.at(i + 3);
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && d >= '0' && d <= '7') {
                out += char(((a - '0') << 6) | ((b - '0') << 3) | (d - '0'));
                i += 3;
                continue;
            }
        }
        out += c;
    }
    return out;
}

bool TmuxControl::parseLayout(const QByteArray &text, PaneLayout &out)
{
    out = PaneLayout();
    if (text.startsWith('{')) {
        const QJsonDocument doc = QJsonDocument::fromJson(text);
        return doc.isObject()
               && parseJsonCell(doc.object().value(QLatin1String("L")).toObject(), out);
    }

    // The classic format: a four-digit checksum, then the root cell.
    const int comma = text.indexOf(',');
    if (comma < 0)
        return false;
    const char *p = text.constData() + comma + 1;
    const char *end = text.constData() + text.size();
    return parseCell(p, end, out) && p == end;
}

void TmuxControl::readOutput()
{
    m_buffer += m_process->readAllStandardOutput();
    int start = 0;
    for (int nl; (nl = m_buffer.indexOf('\n', start)) >= 0; start = nl + 1)
        handleLine(m_buffer.mid(start, nl - start));
    m_buffer.remove(0, start);
}

void TmuxControl::handleLine(QByteArray line)
{
    if (line.endsWith('\r'))
        line.chop(1);

    if (m_inBlock) {
        // A block ends with %end or %error carrying the same command number;
        // anything else, including lines that merely start with '%', is data.
        if (line.startsWith("%end ") || line.startsWith("%error ")) {
            const QList<QByteArray> f = fields(line, 4);
            if (f.size() >= 3 && f.at(2) == m_blockNumber) {
                finishBlock(line.startsWith("%end "));
                return;
            }
        }
        m_blockLines << line;
        return;
    }

    if (line.startsWith("%begin ")) {
        // %begin <time> <number> <flags>: flags bit 1 marks a command this
        // client sent, as opposed to the one tmux was started with.
        const QList<QByteArray> f = fields(line, 4);
        m_inBlock = true;
        m_blockNumber = f.value(2);
        m_blockIsOurs = (f.value(3).toInt() & 1) != 0;
        m_blockLines.clear();
        return;
    }

    if (line.startsWith('%'))
        handleNotification(line);
}

void TmuxControl::finishBlock(bool ok)
{
    m_inBlock = false;
    const QList<QByteArray> lines = m_blockLines;
    m_blockLines.clear();

    if (!m_blockIsOurs) {
        // The startup command's reply: the client is attached.
        if (!m_ready) {
            qCInfo(lcTmux) << "attached";
            m_ready = true;
            emit ready();
        }
        return;
    }
    if (m_pending.isEmpty())
        return;
    if (Reply reply = m_pending.dequeue())
        reply(ok, lines);
}

void TmuxControl::handleNotification(const QByteArray &line)
{
    const int space = line.indexOf(' ');
    const QByteArray name = space < 0 ? line : line.left(space);

    if (name == "%output") {
        // %output %<pane> <escaped data>
        const QList<QByteArray> f = fields(line, 3);
        if (f.size() == 3)
            emit output(parseId(f.at(1)), unescape(f.at(2)));
        else if (f.size() == 2)
            emit output(parseId(f.at(1)), QByteArray());
    } else if (name == "%extended-output") {
        // %extended-output %<pane> <age> … : <escaped data>
        const int sep = line.indexOf(" : ");
        const QList<QByteArray> f = fields(line, 3);
        if (sep > 0 && f.size() >= 2)
            emit output(parseId(f.at(1)), unescape(line.mid(sep + 3)));
    } else if (name == "%layout-change") {
        const QList<QByteArray> f = fields(line, 5);
        if (f.size() >= 3)
            emit layoutChanged(parseId(f.at(1)), f.at(2));
    } else if (name == "%window-add") {
        emit windowAdded(parseId(fields(line, 2).value(1)));
    } else if (name == "%window-close" || name == "%unlinked-window-close") {
        emit windowClosed(parseId(fields(line, 2).value(1)));
    } else if (name == "%window-renamed") {
        const QList<QByteArray> f = fields(line, 3);
        if (f.size() == 3)
            emit windowRenamed(parseId(f.at(1)), QString::fromUtf8(f.at(2)));
    } else if (name == "%session-window-changed") {
        // %session-window-changed $<session> @<window>
        const QList<QByteArray> f = fields(line, 3);
        emit activeWindowChanged(parseId(f.value(1)), parseId(f.value(2)));
    } else if (name == "%window-pane-changed") {
        const QList<QByteArray> f = fields(line, 3);
        emit activePaneChanged(parseId(f.value(1)), parseId(f.value(2)));
    } else if (name == "%session-changed") {
        const QList<QByteArray> f = fields(line, 3);
        if (f.size() == 3)
            emit sessionChanged(parseId(f.at(1)), QString::fromUtf8(f.at(2)));
    } else if (name == "%subscription-changed") {
        // %subscription-changed name $session @window index %pane ... : value
        const int colon = line.indexOf(" : ");
        if (colon > 0) {
            const QList<QByteArray> f = line.left(colon).split(' ');
            const QByteArray pane = f.value(5);
            emit subscriptionChanged(QString::fromUtf8(f.value(1)),
                                     pane.startsWith('%') ? parseId(pane) : -1,
                                     QString::fromUtf8(line.mid(colon + 3)));
        }
    } else if (name == "%pause") {
        emit paused(parseId(fields(line, 2).value(1)));
    } else if (name == "%exit") {
        // %exit [reason]; the process ends right after.
        const QList<QByteArray> f = fields(line, 2);
        if (f.size() == 2)
            m_exitReason = QString::fromUtf8(f.at(1));
    }
    // Others (%sessions-changed, %client-detached, %continue, …) need nothing.
}
