/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "programstatus.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>

namespace {

// The spec's hard limits (rev. 0.4, "Limits"). A report over any is
// discarded whole.
constexpr int kMaxSequence = 4096;   // OSC to ST, so the body less 9 bytes
constexpr int kMaxKey = 16;
constexpr int kMaxMsgEncoded = 2732;
constexpr int kMaxMsgDecoded = 2048;
constexpr int kMaxTitleEncoded = 256;
constexpr int kMaxTitleDecoded = 192;
constexpr int kMaxId = 128;
constexpr int kMaxIdDepth = 8;
constexpr int kMaxRecords = 256;

bool isKeyChar(QChar c) { return c >= QLatin1Char('a') && c <= QLatin1Char('z'); }

bool isValueChar(QChar c)
{
    return (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
           || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringView(u"_.,+/=-").contains(c);
}

/* An id segment and an app share a grammar: [A-Za-z0-9_.+-]{1,32}. */
bool isName(QStringView s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.+-]{1,32}$"));
    return re.matchView(s).hasMatch();
}

bool isId(const QString &id)
{
    if (id.size() > kMaxId)
        return false;
    const QStringList segments = id.split(QLatin1Char('/'));
    if (segments.size() > kMaxIdDepth)
        return false;
    for (const QString &segment : segments) {
        if (!isName(segment))
            return false;
    }
    return true;
}

/* Base64 of UTF-8, one line: nothing if it doesn't decode, is too long, or
 * holds a control character. Invisible formatting (text direction overrides,
 * zero-width characters) is dropped, so it can't disguise what is shown. */
std::optional<QString> decodeText(const QString &encoded, int maxEncoded, int maxDecoded)
{
    // (Qt lets a lone character over a multiple of four through, which no
    // base64 encoder makes: six bits, not a byte.)
    if (encoded.size() > maxEncoded || QStringView(encoded).trimmed().size() % 4 == 1)
        return std::nullopt;
    const auto bytes = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!bytes || bytes.decoded.size() > maxDecoded)
        return std::nullopt;
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8(bytes.decoded);
    if (utf8.hasError())
        return std::nullopt;
    QString out;
    out.reserve(text.size());
    for (QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 || (u >= 0x7f && u <= 0x9f))
            return std::nullopt;
        if (c.category() != QChar::Other_Format)
            out += c;
    }
    return out;
}

ProgramStatusRecord::State stateFrom(const QString &value, bool *clear)
{
    *clear = false;
    if (value == QLatin1String("idle"))
        return ProgramStatusRecord::Idle;
    if (value == QLatin1String("working"))
        return ProgramStatusRecord::Working;
    if (value == QLatin1String("done"))
        return ProgramStatusRecord::Done;
    if (value == QLatin1String("blocked"))
        return ProgramStatusRecord::Blocked;
    if (value == QLatin1String("error"))
        return ProgramStatusRecord::Error;
    if (value == QLatin1String("clear"))
        *clear = true;
    return ProgramStatusRecord::None;
}

/* Whether `id` is `ancestor` or below it. */
bool within(const QString &id, const QString &ancestor)
{
    return id == ancestor
           || (id.size() > ancestor.size() && id.startsWith(ancestor) && id.at(ancestor.size()) == QLatin1Char('/'));
}

} // namespace

std::optional<ProgramStatus::Report> ProgramStatus::parse(const QString &body)
{
    if (body.size() + 9 > kMaxSequence)   // ESC ] 7501 ; ... ESC '\'
        return std::nullopt;

    // key=value pairs between colons; a malformed one is skipped, the last
    // of a key wins.
    QHash<QString, QString> pairs;
    for (const QString &pair : body.split(QLatin1Char(':'))) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = pair.left(eq).trimmed();
        const QString value = pair.mid(eq + 1).trimmed();
        if (key.size() > kMaxKey)
            return std::nullopt;
        if (key.isEmpty() || !std::all_of(key.begin(), key.end(), isKeyChar)
            || !std::all_of(value.begin(), value.end(), isValueChar))
            continue;
        pairs.insert(key, value);
    }

    // A state this doesn't know (one added later) is ignored, not taken as
    // something else.
    Report report;
    ProgramStatusRecord &r = report.record;
    r.state = stateFrom(pairs.value(QStringLiteral("state")), &report.clear);
    if (r.state == ProgramStatusRecord::None && !report.clear)
        return std::nullopt;

    // A bad id is never taken as the root's, which it would overwrite.
    if (pairs.contains(QStringLiteral("id"))) {
        r.id = pairs.value(QStringLiteral("id"));
        if (!isId(r.id))
            return std::nullopt;
    }
    if (report.clear)
        return report;

    const QString app = pairs.value(QStringLiteral("app"));
    if (isName(app))
        r.app = app;
    if (r.state == ProgramStatusRecord::Blocked) {
        const QString kind = pairs.value(QStringLiteral("kind"));
        if (kind == QLatin1String("permission") || kind == QLatin1String("question")
            || kind == QLatin1String("auth"))
            r.kind = kind;
    }
    if (r.state == ProgramStatusRecord::Working || r.state == ProgramStatusRecord::Blocked) {
        static const QRegularExpression percent(QStringLiteral("^(100|[1-9]?[0-9])$"));
        const QString progress = pairs.value(QStringLiteral("progress"));
        if (percent.match(progress).hasMatch())
            r.progress = progress.toInt();
    }
    if (pairs.contains(QStringLiteral("msg"))) {
        const auto msg = decodeText(pairs.value(QStringLiteral("msg")), kMaxMsgEncoded, kMaxMsgDecoded);
        if (!msg)
            return std::nullopt;
        r.msg = *msg;
    }
    if (pairs.contains(QStringLiteral("title"))) {
        const auto title = decodeText(pairs.value(QStringLiteral("title")), kMaxTitleEncoded,
                                      kMaxTitleDecoded);
        if (!title)
            return std::nullopt;
        r.title = *title;
    }
    return report;
}

bool ProgramStatus::apply(const QString &body)
{
    const std::optional<Report> report = parse(body);
    if (!report)
        return false;

    if (report->clear) {
        // No id: everything; otherwise the record and those below it.
        const QString id = report->record.id;
        const qsizetype before = m_records.size();
        m_records.removeIf([&id](const ProgramStatusRecord &r) {
            return id.isEmpty() || within(r.id, id);
        });
        if (m_records.size() != before)
            emit changed();
        return true;
    }

    m_reported = true;
    store(report->record);
    return true;
}

bool ProgramStatus::applyProgress(int state, int progress)
{
    if (m_reported)
        return false;
    if (state == 0) {
        const qsizetype before = m_records.size();
        m_records.removeIf([](const ProgramStatusRecord &r) { return r.id.isEmpty(); });
        if (m_records.size() != before)
            emit changed();
        return true;
    }
    ProgramStatusRecord record;
    switch (state) {
    case 1: case 4: record.state = ProgramStatusRecord::Working; break;
    case 3: record.state = ProgramStatusRecord::Working; progress = -1; break;
    case 2: record.state = ProgramStatusRecord::Error; progress = -1; break;
    default: return false;
    }
    record.progress = progress >= 0 && progress <= 100 ? progress : -1;
    store(record);
    return true;
}

void ProgramStatus::store(ProgramStatusRecord record)
{
    // A report replaces its record whole.
    record.updated = ++m_clock;
    auto it = std::find_if(m_records.begin(), m_records.end(),
                           [&record](const ProgramStatusRecord &r) { return r.id == record.id; });
    if (it != m_records.end()) {
        *it = record;
    } else {
        if (m_records.size() >= kMaxRecords) {
            m_records.erase(std::min_element(m_records.begin(), m_records.end(),
                [](const ProgramStatusRecord &a, const ProgramStatusRecord &b) {
                    return a.updated < b.updated;
                }));
        }
        m_records.append(record);
    }
    emit changed();
}

void ProgramStatus::reset()
{
    m_reported = false;
    if (m_records.isEmpty())
        return;
    m_records.clear();
    emit changed();
}

void ProgramStatus::programEnded()
{
    const qsizetype before = m_records.size();
    m_records.removeIf([](const ProgramStatusRecord &r) { return !r.finished(); });
    if (m_records.size() != before)
        emit changed();
}

void ProgramStatus::markSeen()
{
    bool any = false;
    for (ProgramStatusRecord &r : m_records) {
        if (r.finished() && !r.seen) {
            r.seen = true;
            any = true;
        }
    }
    if (any)
        emit changed();
}

QString ProgramStatus::app(const ProgramStatusRecord &record) const
{
    if (!record.app.isEmpty())
        return record.app;
    // The nearest ancestor's: "a/b/c" looks to "a/b", "a", then the root.
    QString id = record.id;
    while (!id.isEmpty()) {
        const int slash = id.lastIndexOf(QLatin1Char('/'));
        id = slash < 0 ? QString() : id.left(slash);
        for (const ProgramStatusRecord &r : m_records) {
            if (r.id == id && !r.app.isEmpty())
                return r.app;
        }
    }
    return QString();
}

int ProgramStatus::rank(const ProgramStatusRecord &r)
{
    switch (r.state) {
    case ProgramStatusRecord::Blocked: return 4;
    case ProgramStatusRecord::Error: return r.seen ? 0 : 3;
    case ProgramStatusRecord::Done: return r.seen ? 0 : 2;
    case ProgramStatusRecord::Working: return 1;
    default: return 0;
    }
}

ProgramStatusRecord ProgramStatus::mostPressing() const
{
    ProgramStatusRecord best;
    int bestRank = 0;
    for (const ProgramStatusRecord &r : m_records) {
        const int k = rank(r);
        if (k > bestRank || (k == bestRank && k > 0 && r.updated > best.updated)) {
            best = r;
            bestRank = k;
        }
    }
    if (bestRank > 0 && best.app.isEmpty())
        best.app = app(best);
    return best;
}

QString ProgramStatus::label(const ProgramStatusRecord &r)
{
    switch (r.state) {
    case ProgramStatusRecord::Blocked:
        if (r.kind == QLatin1String("permission"))
            return tr("Needs approval");
        if (r.kind == QLatin1String("question"))
            return tr("Question");
        if (r.kind == QLatin1String("auth"))
            return tr("Needs sign-in");
        return tr("Needs you");
    case ProgramStatusRecord::Working:
        return r.progress >= 0 ? tr("Working %1%").arg(r.progress) : tr("Working");
    case ProgramStatusRecord::Done:
        return tr("Done");
    case ProgramStatusRecord::Error:
        return tr("Failed");
    case ProgramStatusRecord::Idle:
        return tr("Idle");
    case ProgramStatusRecord::None:
        break;
    }
    return QString();
}
