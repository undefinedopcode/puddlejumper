/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "commandpalette.h"

#include <QLineEdit>
#include <QListWidget>
#include <QTimer>

namespace {

PaletteDialog::Options paletteOptions()
{
    PaletteDialog::Options o;
    o.placeholder = QObject::tr("Type to find an action");
    o.maxRows = 10;
    return o;
}

}  // namespace

CommandPalette::CommandPalette(QList<Entry> entries, QWidget *parent)
    : PaletteDialog(paletteOptions(), parent)
    , m_entries(std::move(entries))
{
    setWindowTitle(tr("Command Palette"));
    setAttribute(Qt::WA_DeleteOnClose);
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        return a.category == b.category ? a.title.localeAwareCompare(b.title) < 0
                                        : a.category.localeAwareCompare(b.category) < 0;
    });
    setHints(tr("↑↓ Move    ↵ Run    Esc Close"));
    connect(filterField(), &QLineEdit::textChanged, this, &CommandPalette::refilter);
    refilter();
    fitRows();
}

QStringList CommandPalette::shown() const
{
    QStringList out;
    for (int i = 0; i < list()->count(); ++i) {
        const QListWidgetItem *row = list()->item(i);
        if (row->data(KindRole).toInt() == ItemRow)
            out << m_entries.at(row->data(IndexRole).toInt()).name;
    }
    return out;
}

void CommandPalette::refilter()
{
    const QStringList typed = words(filterField()->text());
    list()->clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (!matches(typed, e.title + QLatin1Char(' ') + e.category + QLatin1Char(' ') + e.name + QLatin1Char(' ')
                                + e.keywords))
            continue;
        QListWidgetItem *row = addRow(ItemRow, e.title);
        row->setData(IndexRole, i);
        row->setData(NoteRole, e.category);
        row->setData(DetailRole, e.keys);
        row->setData(MatchRole, typed);
        row->setToolTip(e.name);
    }
    if (list()->count())
        list()->setCurrentRow(0);
    else
        addEmptyRow(tr("No matching actions"));
}

void CommandPalette::activate(QListWidgetItem *row, bool)
{
    if (!row || row->data(KindRole).toInt() != ItemRow)
        return;
    std::function<void()> run = m_entries.value(row->data(IndexRole).toInt()).run;
    accept();
    // After the palette has gone, so the action finds the pane focused again.
    if (run)
        QTimer::singleShot(0, parent(), run);
}
