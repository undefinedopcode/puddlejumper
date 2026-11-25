/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include <functional>

class QLineEdit;
class QListWidget;

/*
 * A command palette: every action there is, filtered as you type, the chosen
 * one run when the palette closes. Words typed must all appear in an entry's
 * title, category, name or keywords.
 */
class CommandPalette : public QDialog
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

    /* The entries the filter shows, in order (for tests). */
    QStringList shown() const;
    void setFilter(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refilter();
    void choose();

    QList<Entry> m_entries;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
};
