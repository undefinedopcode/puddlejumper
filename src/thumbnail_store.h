/*
 * superterm — a modern terminal built on Qt Quick and QMLTermWidget.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QObject>
#include <QString>

/*
 * Supplies filesystem paths for terminal preview thumbnails.
 *
 * QQuickItem::grabToImage() hands back a QQuickItemGrabResult whose .url is an
 * internal "itemgrabber:#N" identifier. Assigning that object to Image.source
 * silently does nothing, so a preview has to be written to a real file and
 * loaded back. These files are ours to manage, so they go in a per-user,
 * per-process directory under the cache location.
 */
class ThumbnailStore : public QObject
{
    Q_OBJECT

public:
    explicit ThumbnailStore(QObject *parent = nullptr);
    ~ThumbnailStore() override;

    /* Path for a given tab id, e.g. .../superterm/thumbs/<pid>/tab-3.png */
    Q_INVOKABLE QString thumbnailPath(int tabId) const;

    /*
     * Versioned path. Each refresh uses a new name because Image caches by
     * URL and file: URLs ignore query strings, so reusing one would display a
     * stale preview.
     */
    Q_INVOKABLE QString thumbnailPathFor(int tabId, int version) const;

private:
    QString m_dir;
};
