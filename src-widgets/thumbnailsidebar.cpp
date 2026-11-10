/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "thumbnailsidebar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Preview images live in this role so they can be updated without rebuilding
// the whole list (which would lose scroll position).
constexpr int kPreviewRole = Qt::UserRole + 1;
constexpr int kTabIndexRole = Qt::UserRole + 2;
} // namespace

ThumbnailSidebar::ThumbnailSidebar(QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(196);

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

    auto makeButton = [header](const QString &text, const QString &tip) {
        auto *b = new QToolButton(header);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };

    auto *addBtn = makeButton(QStringLiteral("＋"), tr("New tab (Ctrl+Shift+T)"));
    auto *layoutBtn = makeButton(QStringLiteral("▥"), tr("Use top tabs"));
    auto *settingsBtn = makeButton(QStringLiteral("⚙"), tr("Settings (Ctrl+,)"));
    headerLayout->addWidget(addBtn);
    headerLayout->addWidget(layoutBtn);
    headerLayout->addWidget(settingsBtn);
    outer->addWidget(header);

    connect(addBtn, &QToolButton::clicked, this, &ThumbnailSidebar::addTabRequested);
    connect(layoutBtn, &QToolButton::clicked, this, &ThumbnailSidebar::layoutToggleRequested);
    connect(settingsBtn, &QToolButton::clicked, this, &ThumbnailSidebar::settingsRequested);

    // ---- list ------------------------------------------------------------
    m_list = new QListWidget(this);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::TopToBottom);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(m_previewSize);
    m_list->setSpacing(4);
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
        if (!item) {
            item = new QListWidgetItem(m_list);
            item->setSizeHint(QSize(m_previewSize.width() + 16,
                                    m_previewSize.height() + 30));
        }
        item->setText(titles.at(i).isEmpty() ? tr("Terminal") : titles.at(i));
        item->setData(kTabIndexRole, i);
        item->setToolTip(item->text());
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
    item->setText(title.isEmpty() ? tr("Terminal") : title);
    item->setToolTip(item->text());
}

void ThumbnailSidebar::rebuildCount()
{
    // Unused placeholder kept for symmetry with future per-card widgets.
}
