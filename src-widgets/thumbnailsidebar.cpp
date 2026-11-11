/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "thumbnailsidebar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace {
// Preview images live in this role so they can be updated without rebuilding
// the whole list (which would lose scroll position).
constexpr int kPreviewRole = Qt::UserRole + 1;
constexpr int kTabIndexRole = Qt::UserRole + 2;
constexpr int kBaseTitleRole = Qt::UserRole + 3;
} // namespace

ThumbnailSidebar::ThumbnailSidebar(QWidget *parent)
    : QWidget(parent)
{
    // Do NOT setFixedWidth here. A fixed width fights the QSplitter: the
    // splitter keeps handing this widget its remembered size (e.g. 256px)
    // while the widget renders only 196px, leaving a dead strip between the
    // sidebar and the terminal that disappears only when the handle is dragged.
    // A minimum width plus the splitter's own sizing is the fix.
    setMinimumWidth(176);
    setMaximumWidth(420);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---- header ----------------------------------------------------------
    auto *header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(8, 6, 6, 6);
    headerLayout->setSpacing(2);

    auto *label = new QLabel(tr("TERMINALS"), header);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    label->setFont(f);
    label->setEnabled(false);
    headerLayout->addWidget(label, 1);

    // New tab, layout toggle and settings live on the main window's toolbar.
    outer->addWidget(header);

    // ---- list ------------------------------------------------------------
    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::TopToBottom);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(m_previewSize);
    m_list->setSpacing(4);
    // Cards should span the sidebar width rather than sitting at a fixed size
    // with dead space to their right.
    m_list->setWordWrap(false);
    m_list->setTextElideMode(Qt::ElideMiddle);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(m_list, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0)
            emit tabActivated(row);
    });
}

void ThumbnailSidebar::setPreviewSize(const QSize &size)
{
    m_previewSize = size;
    m_list->setIconSize(size);
}

void ThumbnailSidebar::syncTabs(const QStringList &titles, int currentIndex)
{
    // Block signals so programmatic selection does not look like a user click
    // and bounce the current tab back and forth.
    const QSignalBlocker blocker(m_list);

    while (m_list->count() > titles.size())
        delete m_list->takeItem(m_list->count() - 1);

    for (int i = 0; i < titles.size(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (!item)
            item = new QListWidgetItem(m_list);
        const QString base = titles.at(i).isEmpty() ? tr("Terminal") : titles.at(i);
        item->setData(kBaseTitleRole, base);
        item->setData(kTabIndexRole, i);
        item->setSizeHint(itemSize());
        // Keep any busy marker the item already has.
        setBusy(i, item->text().startsWith(QStringLiteral("\u25cf ")));
    }

    if (currentIndex >= 0 && currentIndex < m_list->count())
        m_list->setCurrentRow(currentIndex);
}

void ThumbnailSidebar::setPreview(int index, const QPixmap &pixmap)
{
    if (index < 0 || index >= m_list->count() || pixmap.isNull())
        return;
    m_list->item(index)->setIcon(QIcon(pixmap));
}

void ThumbnailSidebar::setCurrentIndex(int index)
{
    const QSignalBlocker blocker(m_list);
    if (index >= 0 && index < m_list->count())
        m_list->setCurrentRow(index);
}

void ThumbnailSidebar::setTitle(int index, const QString &title)
{
    if (index < 0 || index >= m_list->count())
        return;
    QListWidgetItem *item = m_list->item(index);
    const QString base = title.isEmpty() ? tr("Terminal") : title;
    const bool wasBusy = item->text().startsWith(QStringLiteral("\u25cf "));
    item->setData(kBaseTitleRole, base);
    setBusy(index, wasBusy);
}

// Card size that spans the current viewport width, so cards do not sit at a
// fixed size with an empty strip beside them.
QSize ThumbnailSidebar::itemSize() const
{
    const int margin = 8;
    int width = m_previewSize.width();
    if (m_list && m_list->viewport())
        width = qMax(64, m_list->viewport()->width() - margin);
    return QSize(width, m_previewSize.height() + 30);
}

void ThumbnailSidebar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Item width tracks the viewport, so re-apply hints when we change width.
    if (!m_list)
        return;
    const QSize sz = itemSize();
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setSizeHint(sz);
}

void ThumbnailSidebar::setBusy(int index, bool busy)
{
    if (index < 0 || index >= m_list->count())
        return;
    QListWidgetItem *item = m_list->item(index);
    // A dot before the label marks a tab that produced output while you were
    // not looking at it. Cheap, and it survives without extra widgets.
    const QString base = item->data(kBaseTitleRole).toString();
    const QString title = base.isEmpty() ? tr("Terminal") : base;
    item->setText(busy ? QStringLiteral("\u25cf ") + title : title);
    item->setToolTip(busy ? tr("%1 — new output").arg(title) : title);
}
