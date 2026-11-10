/*
 * superterm — a modern terminal built on Qt Quick and QMLTermWidget.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "thumbnail_store.h"

#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>

ThumbnailStore::ThumbnailStore(QObject *parent)
    : QObject(parent)
{
    // Keyed by pid so two running instances never overwrite each other's
    // previews, and left behind for the OS to clean up rather than deleted on
    // exit: tearing files down while an async Image load may still be in
    // flight is not worth the risk for a handful of PNGs.
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    m_dir = base + QStringLiteral("/thumbs/%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(m_dir);
}

ThumbnailStore::~ThumbnailStore() = default;

QString ThumbnailStore::thumbnailPath(int tabId) const
{
    return m_dir + QStringLiteral("/tab-%1.png").arg(tabId);
}

QString ThumbnailStore::thumbnailPathFor(int tabId, int version) const
{
    // A distinct filename per version: QML's Image caches by URL, and file:
    // URLs do not honour query strings, so reusing a name would show a stale
    // preview.
    return m_dir + QStringLiteral("/tab-%1-%2.png").arg(tabId).arg(version);
}
