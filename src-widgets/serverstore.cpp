/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "serverstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUrl>
#include <QUuid>

namespace {
constexpr auto kKey = "servers/list";

// An address as typed, for comparing: Rex's scheme and a trailing slash
// don't make a different server.
QString normalRex(const QString &address)
{
    QString a = address.trimmed();
    if (!a.contains(QLatin1String("://")))
        a.prepend(QLatin1String("http://"));
    const QUrl url(a);
    if (!url.isValid() || url.host().isEmpty())
        return a;
    const int port = url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
    return url.scheme() + QLatin1String("://") + url.host().toLower() + QLatin1Char(':') + QString::number(port);
}
} // namespace

QString SavedServer::kindName(Kind kind)
{
    switch (kind) {
    case Rex: return QObject::tr("Rex");
    case Tmux: return QObject::tr("tmux over ssh");
    case Vnc: return QObject::tr("VNC");
    case Rdp: return QObject::tr("RDP");
    case Web: return QObject::tr("Web");
    }
    return {};
}

QString SavedServer::kindKey(Kind kind)
{
    switch (kind) {
    case Rex: return QStringLiteral("rex");
    case Tmux: return QStringLiteral("tmux");
    case Vnc: return QStringLiteral("vnc");
    case Rdp: return QStringLiteral("rdp");
    case Web: return QStringLiteral("web");
    }
    return {};
}

SavedServer::Kind SavedServer::kindFromKey(const QString &key, bool *ok)
{
    for (Kind k : {Rex, Tmux, Vnc, Rdp, Web}) {
        if (kindKey(k) == key) {
            if (ok)
                *ok = true;
            return k;
        }
    }
    if (ok)
        *ok = false;
    return Rex;
}

ServerStore::ServerStore(QObject *parent)
    : QObject(parent)
{
    load();
}

void ServerStore::load()
{
    m_servers.clear();
    const QJsonArray list = QJsonDocument::fromJson(QSettings().value(QLatin1String(kKey)).toString().toUtf8()).array();
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        bool ok = false;
        SavedServer s;
        s.kind = SavedServer::kindFromKey(o.value(QLatin1String("kind")).toString(), &ok);
        if (!ok)
            continue;  // a kind this build doesn't know
        s.id = o.value(QLatin1String("id")).toString();
        if (s.id.isEmpty())
            s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        s.name = o.value(QLatin1String("name")).toString();
        s.address = o.value(QLatin1String("address")).toString();
        s.session = o.value(QLatin1String("session")).toString();
        s.user = o.value(QLatin1String("user")).toString();
        s.reducedColour = o.value(QLatin1String("reducedColour")).toBool();
        s.adaptive = o.value(QLatin1String("adaptive")).toBool();
        m_servers << s;
    }
}

void ServerStore::write()
{
    QJsonArray list;
    for (const SavedServer &s : std::as_const(m_servers)) {
        QJsonObject o{{QStringLiteral("id"), s.id},
                      {QStringLiteral("name"), s.name},
                      {QStringLiteral("kind"), SavedServer::kindKey(s.kind)},
                      {QStringLiteral("address"), s.address}};
        if (!s.session.isEmpty())
            o.insert(QStringLiteral("session"), s.session);
        if (!s.user.isEmpty())
            o.insert(QStringLiteral("user"), s.user);
        if (s.reducedColour)
            o.insert(QStringLiteral("reducedColour"), true);
        if (s.adaptive)
            o.insert(QStringLiteral("adaptive"), true);
        list.append(o);
    }
    QSettings().setValue(QLatin1String(kKey), QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact)));
    emit changed();
}

SavedServer ServerStore::server(const QString &id) const
{
    for (const SavedServer &s : m_servers)
        if (s.id == id)
            return s;
    return {};
}

QString ServerStore::save(SavedServer server)
{
    if (server.id.isEmpty())
        server.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool replaced = false;
    for (SavedServer &s : m_servers) {
        if (s.id == server.id) {
            s = server;
            replaced = true;
        }
    }
    if (!replaced)
        m_servers << server;
    write();
    return server.id;
}

void ServerStore::remove(const QString &id)
{
    const auto n = m_servers.removeIf([&](const SavedServer &s) { return s.id == id; });
    if (n)
        write();
}

void ServerStore::move(const QString &id, int step)
{
    for (int i = 0; i < m_servers.size(); ++i) {
        if (m_servers.at(i).id != id)
            continue;
        const int to = i + step;
        if (to < 0 || to >= m_servers.size())
            return;
        m_servers.move(i, to);
        write();
        return;
    }
}

SavedServer ServerStore::rexServer(const QString &address) const
{
    const QString want = normalRex(address);
    for (const SavedServer &s : m_servers)
        if (s.kind == SavedServer::Rex && normalRex(s.address) == want)
            return s;
    return {};
}
