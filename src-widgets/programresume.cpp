/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "programresume.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr int kMaxArgs = 1024;   // the spec asks for at least 64
constexpr qint64 kWindow = 60;   // seconds a report's time may be from now

bool isKey(const QString &k)
{
    if (k.isEmpty())
        return false;
    for (const QChar c : k)
        if (c < QLatin1Char('a') || c > QLatin1Char('z'))
            return false;
    return true;
}

bool isValue(const QString &v)
{
    static const QString extra = QStringLiteral("_.,+/=-");
    for (const QChar c : v) {
        const bool ok = (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || extra.contains(c);
        if (!ok)
            return false;
    }
    return true;
}

bool isDecimal(const QString &s)
{
    if (s.isEmpty() || s.at(0) == QLatin1Char('0'))
        return false;
    for (const QChar c : s)
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    return true;
}

// Base64 (padding optional) of UTF-8 without control characters.
bool decodeText(const QString &v, QString *out)
{
    const auto r = QByteArray::fromBase64Encoding(v.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!r)
        return false;
    const QByteArray raw = *r;
    const QString s = QString::fromUtf8(raw);
    if (s.toUtf8() != raw)
        return false;   // not UTF-8
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u <= 0x9f))
            return false;
    }
    *out = s;
    return true;
}

} // namespace

QString ProgramResume::newToken()
{
    QByteArray b(24, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(b.data()), b.size() / 4);
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

ProgramResume::Verdict ProgramResume::parse(const QString &body, const QString &token, qint64 now, Report *rep)
{
    *rep = Report();
    if (token.isEmpty() || body == QLatin1String("?"))
        return Verdict::Ignore;
    const int i = body.lastIndexOf(QLatin1String(":mac="));
    if (i < 0)
        return Verdict::Ignore;
    const QString signedPart = body.left(i), mac = body.mid(i + 5);
    if (mac.size() != 64 || mac.toLower() != mac)
        return Verdict::Ignore;
    const QByteArray want = QCryptographicHash::hash((signedPart + QLatin1Char(':') + token).toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    // In constant time.
    const QByteArray got = mac.toLatin1();
    unsigned char diff = 0;
    for (int k = 0; k < 64; ++k)
        diff |= static_cast<unsigned char>(got.at(k) ^ want.at(k));
    if (diff != 0)
        return Verdict::Ignore;

    // Authenticated. The pid and time first: without them, nothing.
    QHash<QString, QString> pairs;
    bool malformed = false;
    for (const QString &p : signedPart.split(QLatin1Char(':'))) {
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq < 0 || !isKey(p.left(eq)) || !isValue(p.mid(eq + 1))) {
            malformed = true;
            continue;
        }
        if (pairs.contains(p.left(eq)))
            malformed = true;
        pairs.insert(p.left(eq), p.mid(eq + 1));
    }
    if (!isDecimal(pairs.value(QStringLiteral("pid"))) || !isDecimal(pairs.value(QStringLiteral("time"))))
        return Verdict::Ignore;
    bool ok1 = false, ok2 = false;
    rep->pid = pairs.value(QStringLiteral("pid")).toInt(&ok1);
    rep->time = pairs.value(QStringLiteral("time")).toLongLong(&ok2);
    if (!ok1 || !ok2)
        return Verdict::Ignore;
    if (qAbs(now - rep->time) > kWindow)
        return Verdict::Ignore;
    if (malformed)
        return Verdict::Reject;
    static const QSet<QString> known = {QStringLiteral("pid"), QStringLiteral("time"), QStringLiteral("argv"),
                                        QStringLiteral("cwd"), QStringLiteral("clear"), QStringLiteral("need")};
    if (pairs.contains(QStringLiteral("need")))
        for (const QString &k : pairs.value(QStringLiteral("need")).split(QLatin1Char(',')))
            if (!known.contains(k))
                return Verdict::Reject;
    const bool hasArgv = pairs.contains(QStringLiteral("argv")), hasClear = pairs.contains(QStringLiteral("clear"));
    if (hasArgv == hasClear)
        return Verdict::Reject;
    if (hasClear)
        return pairs.value(QStringLiteral("clear")) == QLatin1String("1") ? Verdict::Clear : Verdict::Reject;
    const QStringList parts = pairs.value(QStringLiteral("argv")).split(QLatin1Char(','));
    if (parts.size() > kMaxArgs)
        return Verdict::Reject;
    for (int k = 0; k < parts.size(); ++k) {
        QString s;
        if (!decodeText(parts.at(k), &s) || (k == 0 && s.isEmpty()))
            return Verdict::Reject;
        rep->argv << s;
    }
    // The program: a bare name (found on PATH) or an absolute path.
    const QString &prog = rep->argv.first();
    if (prog.startsWith(QLatin1Char('-')) || (prog.contains(QLatin1Char('/')) && !prog.startsWith(QLatin1Char('/'))))
        return Verdict::Reject;
    if (pairs.contains(QStringLiteral("cwd"))) {
        QString s;
        if (!decodeText(pairs.value(QStringLiteral("cwd")), &s) || !s.startsWith(QLatin1Char('/')))
            return Verdict::Reject;
        rep->cwd = s;
    }
    return Verdict::Set;
}

bool ProgramResume::owns(int pid, int leader)
{
#ifdef Q_OS_UNIX
    if (pid <= 0 || leader <= 0)
        return false;
    const pid_t sid = ::getsid(pid);
    return sid >= 0 && sid == leader && ::kill(pid, 0) == 0;
#else
    Q_UNUSED(pid);
    Q_UNUSED(leader);
    return false;
#endif
}

bool ProgramResume::apply(const QString &body, int leader)
{
    Report rep;
    const Verdict v = parse(body, m_token, QDateTime::currentSecsSinceEpoch(), &rep);
    if (v == Verdict::Ignore || !owns(rep.pid, leader))
        return false;
    if (m_lastTime.contains(rep.pid) && rep.time < m_lastTime.value(rep.pid))
        return false;   // older than one already taken
    switch (v) {
    case Verdict::Reject:
        return m_records.remove(rep.pid) > 0;
    case Verdict::Clear:
        m_lastTime.insert(rep.pid, rep.time);
        return m_records.remove(rep.pid) > 0;
    case Verdict::Set:
        m_lastTime.insert(rep.pid, rep.time);
        m_records.insert(rep.pid, Record{rep.pid, rep.argv, rep.cwd, rep.time, ++m_order});
        return true;
    case Verdict::Ignore:
        break;
    }
    return false;
}

QList<ProgramResume::Record> ProgramResume::records(int leader)
{
    for (auto it = m_records.begin(); it != m_records.end();) {
        if (!owns(it.key(), leader))
            it = m_records.erase(it);
        else
            ++it;
    }
    QList<Record> out = m_records.values();
    std::sort(out.begin(), out.end(), [](const Record &a, const Record &b) { return a.order > b.order; });
    return out;
}

#ifdef Q_OS_UNIX
// See programresume.h.
int ProgramResume::runHelper(const char *path)
{
    QFile f(QString::fromLocal8Bit(path));
    const QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    f.close();
    QFile::remove(f.fileName());
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    const QJsonArray argv = o.value(QStringLiteral("argv")).toArray();
    if (argv.isEmpty()) {
        fprintf(stderr, "puddlejumper: nothing to resume\n");
        return 1;
    }
    QList<QByteArray> args;
    for (const QJsonValue &a : argv)
        args << a.toString().toLocal8Bit();
    const QByteArray cwd = o.value(QStringLiteral("cwd")).toString().toLocal8Bit();
    if (!cwd.isEmpty() && ::chdir(cwd.constData()) != 0) {
        fprintf(stderr, "puddlejumper: not resuming %s: %s: %s\n", args.first().constData(), cwd.constData(),
                strerror(errno));
        return 1;
    }
    std::vector<char *> raw;
    for (QByteArray &a : args)
        raw.push_back(a.data());
    raw.push_back(nullptr);
    ::execvp(raw.front(), raw.data());
    fprintf(stderr, "puddlejumper: not resuming %s: %s\n", raw.front(), strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
#endif
#ifndef Q_OS_UNIX
int ProgramResume::runHelper(const char *)
{
    return 1;
}
#endif
