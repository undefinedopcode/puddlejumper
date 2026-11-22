#pragma once

/*
 * PuddleJumper was called superterm. What someone has from that time (its
 * environment variables, settings, workspaces and colour schemes) still works.
 */

#include <QByteArray>
#include <QString>

// PUDDLEJUMPER_<name> or, unset, the former SUPERTERM_<name>.
inline QByteArray appEnv(const char *name)
{
    const QByteArray now = QByteArrayLiteral("PUDDLEJUMPER_") + name;
    if (qEnvironmentVariableIsSet(now.constData()))
        return qgetenv(now.constData());
    return qgetenv((QByteArrayLiteral("SUPERTERM_") + name).constData());
}

inline QString appEnvString(const char *name)
{
    return QString::fromLocal8Bit(appEnv(name));
}

inline bool appEnvIsSet(const char *name)
{
    return qEnvironmentVariableIsSet((QByteArrayLiteral("PUDDLEJUMPER_") + name).constData())
        || qEnvironmentVariableIsSet((QByteArrayLiteral("SUPERTERM_") + name).constData());
}

/*
 * Carry superterm's settings, workspaces and colour schemes over, the first
 * time PuddleJumper runs: once the application's names are set, before
 * anything reads them. What was there is left as it was.
 */
void adoptFormerName();
