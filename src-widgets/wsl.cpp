/*
 * superterm — the Windows Subsystem for Linux. See wsl.h.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "wsl.h"

#include <QSettings>

namespace wsl {

QStringList distributions()
{
    QStringList names;
#ifdef Q_OS_WIN
    // Where WSL keeps them, one key each. ("wsl --list" says the same, but
    // slowly, and in UTF-16 with decoration.)
    QSettings lxss(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Lxss"),
                   QSettings::NativeFormat);
    const QString defaultId = lxss.value(QStringLiteral("DefaultDistribution")).toString();
    const QStringList ids = lxss.childGroups();
    for (const QString &id : ids) {
        const QString name = lxss.value(id + QStringLiteral("/DistributionName")).toString();
        // State 1 is installed; others are being installed or removed.
        if (name.isEmpty() || lxss.value(id + QStringLiteral("/State")).toInt() != 1)
            continue;
        if (id == defaultId)
            names.prepend(name);
        else
            names.append(name);
    }
#endif
    return names;
}

QStringList shellCommand(const QString &distribution)
{
    // (Without --cd it would start in our own directory, as /mnt/c/...)
    return {QStringLiteral("wsl.exe"), QStringLiteral("-d"), distribution,
            QStringLiteral("--cd"), QStringLiteral("~")};
}

}
