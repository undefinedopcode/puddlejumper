/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "commandpalette.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

CommandPalette::CommandPalette(QList<Entry> entries, QWidget *parent)
    : QDialog(parent)
    , m_entries(std::move(entries))
{
    setWindowTitle(tr("Command Palette"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Type to find an action"));
    m_filter->setClearButtonEnabled(true);
    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_filter);
    layout->addWidget(m_list);
    resize(560, 420);
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        return a.category == b.category ? a.title.localeAwareCompare(b.title) < 0
                                        : a.category.localeAwareCompare(b.category) < 0;
    });
    connect(m_filter, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(m_filter, &QLineEdit::returnPressed, this, &CommandPalette::choose);
    connect(m_list, &QListWidget::itemActivated, this, &CommandPalette::choose);
    m_filter->installEventFilter(this);
    refilter();
}

QStringList CommandPalette::shown() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        out << m_list->item(i)->data(Qt::UserRole + 1).toString();
    return out;
}

void CommandPalette::setFilter(const QString &text)
{
    m_filter->setText(text);
}

bool CommandPalette::eventFilter(QObject *watched, QEvent *event)
{
    // Up and Down move through the list while typing.
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_PageDown || key == Qt::Key_PageUp) {
            QCoreApplication::sendEvent(m_list, event);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CommandPalette::refilter()
{
    const QStringList words = m_filter->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    m_list->clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        const QString hay = e.title + QLatin1Char(' ') + e.category + QLatin1Char(' ') + e.name
                            + QLatin1Char(' ') + e.keywords;
        bool all = true;
        for (const QString &w : words)
            all = all && hay.contains(w, Qt::CaseInsensitive);
        if (!all)
            continue;
        QString text = e.category.isEmpty() ? e.title : e.category + QStringLiteral(": ") + e.title;
        if (!e.keys.isEmpty())
            text += QStringLiteral("    ") + e.keys;
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, i);
        item->setData(Qt::UserRole + 1, e.name);
        item->setToolTip(e.name);
    }
    if (m_list->count())
        m_list->setCurrentRow(0);
}

void CommandPalette::choose()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    std::function<void()> run = m_entries.value(item->data(Qt::UserRole).toInt()).run;
    accept();
    // After the palette has gone, so the action finds the pane focused again.
    if (run)
        QTimer::singleShot(0, parent(), run);
}
