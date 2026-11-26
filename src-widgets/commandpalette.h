/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "palette.h"

#include <QList>
#include <QString>

#include <functional>

/*
 * A command palette: every action there is, filtered as you type, the chosen
 * one run when the palette closes. Words typed must all appear in an entry's
 * title, category, name or keywords. It looks as a Rex prompt does
 * (palette.h): each row the action's title, its category after it in
 * quieter text, and its keys in a pill at the far end.
 */
class CommandPalette : public PaletteDialog
{
    Q_OBJECT

public:
    struct Entry
    {
        QString title;
        QString category;
        QString name;
        QString keywords;
        QString keys;  // shown, for reference
        std::function<void()> run;
    };

    explicit CommandPalette(QList<Entry> entries, QWidget *parent = nullptr);

    /* The entries the filter shows, in order, by name (for tests). */
    QStringList shown() const;

protected:
    void activate(QListWidgetItem *row, bool confirm) override;

private:
    void refilter();

    QList<Entry> m_entries;
};
