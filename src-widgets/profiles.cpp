/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "profiles.h"

#include <QSettings>
#include <QUuid>

namespace {
constexpr auto kOrder        = "profiles/order";
constexpr auto kDefaultId    = "profiles/default";
constexpr auto kPrefix       = "profiles/";

QString keyFor(const QString &id, const char *field)
{
    return QStringLiteral("profiles/%1/%2").arg(id, QLatin1String(field));
}

QString newId()
{
    // Short and readable in the config file; collision odds are irrelevant at
    // this scale but the loop below guards them anyway.
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}
} // namespace

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent)
{
    load();
}

const TerminalProfile &ProfileStore::defaultProfile() const
{
    for (const auto &p : m_profiles) {
        if (p.id == m_defaultId)
            return p;
    }
    // load() guarantees a default, so this is only reachable if the invariant
    // is ever broken; returning the first profile keeps the app running.
    return m_profiles.first();
}

const TerminalProfile &ProfileStore::profileById(const QString &id) const
{
    for (const auto &p : m_profiles) {
        if (p.id == id)
            return p;
    }
    return defaultProfile();
}

QString ProfileStore::add(const TerminalProfile &profile)
{
    TerminalProfile p = profile;

    // Never trust a caller-supplied id: ids are ours to mint, and a duplicate
    // would make profileById() ambiguous.
    QString id = p.id;
    auto taken = [this](const QString &candidate) {
        for (const auto &e : m_profiles) {
            if (e.id == candidate)
                return true;
        }
        return false;
    };
    while (id.isEmpty() || taken(id))
        id = newId();
    p.id = id;

    // Only one profile may be the default, and it is chosen explicitly.
    p.isDefault = false;
    if (p.name.trimmed().isEmpty())
        p.name = tr("Profile");

    m_profiles.append(p);
    save();
    emit profilesChanged();
    return id;
}

QString ProfileStore::duplicate(const QString &sourceId)
{
    const TerminalProfile *source = nullptr;
    for (const auto &p : m_profiles) {
        if (p.id == sourceId) {
            source = &p;
            break;
        }
    }
    if (!source)
        return QString();

    TerminalProfile copy = *source;
    // Pick a name that does not collide, so the list stays readable.
    QString base = tr("%1 copy").arg(source->name);
    QString name = base;
    int n = 2;
    auto nameTaken = [this](const QString &candidate) {
        for (const auto &e : m_profiles) {
            if (e.name.compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    while (nameTaken(name))
        name = tr("%1 %2").arg(base).arg(n++);
    copy.name = name;

    return add(copy);
}

bool ProfileStore::update(const TerminalProfile &profile)
{
    for (auto &p : m_profiles) {
        if (p.id != profile.id)
            continue;

        const bool wasDefault = p.isDefault;
        p = profile;
        // The default flag is owned by the store, not by the editor.
        p.isDefault = wasDefault;
        if (p.name.trimmed().isEmpty())
            p.name = tr("Profile");

        save();
        emit profilesChanged();
        return true;
    }
    return false;
}

bool ProfileStore::remove(const QString &id)
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles.at(i).id != id)
            continue;

        // The default profile is the fallback for every dangling reference, so
        // it must survive.
        if (id == m_defaultId)
            return false;

        m_profiles.removeAt(i);
        QSettings s;
        s.remove(QStringLiteral("profiles/%1").arg(id));
        save();
        emit profilesChanged();
        return true;
    }
    return false;
}

void ProfileStore::load()
{
    QSettings s;

    const QStringList order =
        s.value(QLatin1String(kOrder)).toStringList();

    for (const QString &id : order) {
        if (id.isEmpty())
            continue;
        TerminalProfile p;
        p.id = id;
        p.name = s.value(keyFor(id, "name")).toString();
        if (p.name.isEmpty())
            continue;   // orphaned order entry

        auto readOptString = [&s, &id](const char *field) -> std::optional<QString> {
            const QString key = keyFor(id, field);
            if (!s.contains(key))
                return std::nullopt;
            return s.value(key).toString();
        };
        auto readOptDouble = [&s, &id](const char *field) -> std::optional<double> {
            const QString key = keyFor(id, field);
            if (!s.contains(key))
                return std::nullopt;
            return s.value(key).toDouble();
        };
        auto readOptList = [&s, &id](const char *field) -> std::optional<QStringList> {
            const QString key = keyFor(id, field);
            if (!s.contains(key))
                return std::nullopt;
            return s.value(key).toStringList();
        };

        p.colorScheme      = readOptString("colorScheme");
        p.fontFamily       = readOptString("fontFamily");
        p.fontSize         = readOptDouble("fontSize");
        p.shellProgram     = readOptString("shellProgram");
        p.shellArgs        = readOptList("shellArgs");
        p.workingDirectory = readOptString("workingDirectory");

        m_profiles.append(p);
    }

    m_defaultId = s.value(QLatin1String(kDefaultId)).toString();

    // A first run (or a config edited by hand into nothing) gets a default
    // profile. It carries no overrides, so it behaves exactly like the old
    // single-config app.
    if (m_profiles.isEmpty()) {
        TerminalProfile def;
        def.id = newId();
        def.name = tr("Default");
        def.isDefault = true;
        m_profiles.append(def);
        m_defaultId = def.id;
        save();
    }

    bool defaultFound = false;
    for (auto &p : m_profiles) {
        p.isDefault = (p.id == m_defaultId);
        if (p.isDefault)
            defaultFound = true;
    }
    // The stored default id may name a profile that has since been deleted or
    // hand-edited away; fall back to the first.
    if (!defaultFound) {
        m_profiles.first().isDefault = true;
        m_defaultId = m_profiles.first().id;
        save();
    }
}

void ProfileStore::save()
{
    QSettings s;

    // Rewrite the whole group so deleted profiles leave no keys behind.
    s.remove(QStringLiteral("profiles"));

    QStringList order;
    for (const auto &p : m_profiles) {
        order << p.id;
        s.setValue(keyFor(p.id, "name"), p.name);
        // Write only the fields that are actually set: an inheriting profile
        // stays minimal, and later global changes still reach it.
        if (p.colorScheme)
            s.setValue(keyFor(p.id, "colorScheme"), *p.colorScheme);
        if (p.fontFamily)
            s.setValue(keyFor(p.id, "fontFamily"), *p.fontFamily);
        if (p.fontSize)
            s.setValue(keyFor(p.id, "fontSize"), *p.fontSize);
        if (p.shellProgram)
            s.setValue(keyFor(p.id, "shellProgram"), *p.shellProgram);
        if (p.shellArgs)
            s.setValue(keyFor(p.id, "shellArgs"), *p.shellArgs);
        if (p.workingDirectory)
            s.setValue(keyFor(p.id, "workingDirectory"), *p.workingDirectory);
    }
    s.setValue(QLatin1String(kOrder), order);
    s.setValue(QLatin1String(kDefaultId), m_defaultId);
    s.sync();
}
