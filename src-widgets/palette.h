/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

/*
 * A palette, as Rex's apps show one: a frameless panel across the top of the
 * window, an optional heading over a large text field, a list of rows below
 * and a line of key hints. The command palette and a Rex server's prompts
 * (rex.ui.input, rex.ui.pick) are both one.
 *
 * A row is drawn from its data (the roles below): a tick circle (with
 * `ticks`), an icon (a Lucide name), the title with the filter's words in
 * bold, a note in quieter text after it, a subtitle on a second line, and a
 * detail (a short status, or keys) in a pill at the far end.
 *
 * The field keeps the keys: Up, Down, Page Up and Page Down move through the
 * rows, Return activates the current one (with Ctrl, or Cmd on macOS,
 * `confirm`), Esc rejects. A click activates the row clicked.
 */
class PaletteDialog : public QDialog
{
    Q_OBJECT

public:
    enum Role {
        KindRole = Qt::UserRole,  // RowKind
        IndexRole,                // the subclass's own index
        TitleRole,
        SubtitleRole,
        NoteRole,
        IconRole,  // a Lucide name (appicon.h)
        DetailRole,
        TickedRole,
        MatchRole,  // the filter's words, emboldened in the title
    };
    enum RowKind { ItemRow, UseRow, EmptyRow };

    struct Options
    {
        QString heading;        // over the field; none if empty
        QString placeholder;
        bool searchIcon = true;
        bool ticks = false;     // rows have tick circles
        int maxRows = 8;        // shown before the list scrolls
    };

    QLineEdit *filterField() const { return m_filter; }
    /* For tests: the rows' titles, and the current one's. */
    QStringList shownTitles() const;
    QString currentTitle() const;
    void setFilter(const QString &text);

protected:
    PaletteDialog(const Options &options, QWidget *parent);

    /* Rows: an item (data set on it after), and the one row when nothing
     * matches, which can't be chosen. */
    QListWidgetItem *addRow(RowKind kind, const QString &title);
    void addEmptyRow(const QString &text);
    /* After the first rows: the list's height for up to maxRows of them,
     * kept while filtering so the palette doesn't jump. */
    void fitRows();
    void setHints(const QString &text);
    QHBoxLayout *footer() const { return m_footer; }
    QListWidget *list() const { return m_list; }

    /* Return (confirm: with Ctrl/Cmd) or a click on a row (or none). */
    virtual void activate(QListWidgetItem *row, bool confirm) = 0;

    static QStringList words(const QString &typed);
    static bool matches(const QStringList &words, const QString &haystack);
    /* "⌘↵" on macOS, "Ctrl+↵" elsewhere. */
    static QString confirmKeys();

    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void step(int by);

    int m_maxRows;
    QLabel *m_heading = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_hints = nullptr;
    QHBoxLayout *m_footer = nullptr;
};
