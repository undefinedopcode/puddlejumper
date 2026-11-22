#include "formername.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

const QString kFormer = QStringLiteral("superterm");

// A copy of the folder `from` as `to`, unless `to` is there already.
void copyTree(const QString &from, const QString &to)
{
    if (!QFileInfo(from).isDir() || QFileInfo::exists(to))
        return;
    QDir().mkpath(to);
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString target = to + QDir(from).relativeFilePath(path).prepend(QLatin1Char('/'));
        if (it.fileInfo().isDir())
            QDir().mkpath(target);
        else
            QFile::copy(path, target);
    }
}

} // namespace

void adoptFormerName()
{
    // The settings, wherever this platform keeps them (a file, a property
    // list on macOS, the registry on Windows).
    QSettings now;
    if (now.allKeys().isEmpty()) {
        QSettings before(kFormer, kFormer);
        const QStringList keys = before.allKeys();
        for (const QString &key : keys)
            now.setValue(key, before.value(key));
        now.sync();
    }

    // Its colour schemes, where the platform keeps an application's data:
    // asked for under the former names.
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString organisation = QCoreApplication::organizationName();
    const QString application = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(kFormer);
    QCoreApplication::setApplicationName(kFormer);
    const QString formerData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setOrganizationName(organisation);
    QCoreApplication::setApplicationName(application);
    copyTree(formerData, data);

    // Its workspaces.
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    copyTree(config + QLatin1Char('/') + kFormer + QStringLiteral("/workspaces"),
             config + QStringLiteral("/puddlejumper/workspaces"));
}
