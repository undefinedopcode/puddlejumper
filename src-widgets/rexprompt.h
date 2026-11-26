/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/*
 * A Rex server's question (client_prompt_requested: an action's
 * rex.ui.input or rex.ui.pick), asked as Rex's apps ask it: a palette over
 * the window, its title above a text field.
 *
 *   input: the field, and one row to submit it, reading the prompt's
 *          `confirm` or Use “…” with the text.
 *   pick:  the field filters the items (every word typed, in any of
 *          title, subtitle, id or detail); each row an icon, the title, a
 *          quieter subtitle beneath and a short detail at the far end.
 *          Return chooses. With `multiple`, Return ticks and Cmd+Return
 *          (Ctrl+Return) confirms, the tick count beside the confirm label;
 *          with `allow_other`, a Use “…” row answers with the text typed.
 *
 * Accepted, answer() is what client.reply sends: {text} or {ids}.
 * Rejected (Esc, or closed): the caller answers {cancelled: true}.
 */
class RexPrompt : public QDialog
{
    Q_OBJECT

public:
    explicit RexPrompt(const QJsonObject &prompt, QWidget *parent = nullptr);

    QJsonObject answer() const { return m_answer; }

    /* The icon names Rex's prompts take ("folder", "branch", …). */
    static QStringList iconNames();

    /* For tests: the filter, the rows shown (titles), the current one,
     * and the ids ticked. */
    void setFilter(const QString &text);
    QStringList shownTitles() const;
    QString currentTitle() const;
    QStringList tickedIds() const;
    QLineEdit *filterField() const { return m_filter; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Item
    {
        QString id, title, subtitle, icon, detail;
        bool ticked = false;
    };
    void refilter();
    void activate(QListWidgetItem *row, bool confirm);
    void step(int by);
    void updateConfirm();
    void finish(const QJsonObject &answer);
    QString confirmKeys() const;

    bool m_pick = false;
    bool m_multiple = false;
    bool m_allowOther = false;
    QString m_confirm;
    QList<Item> m_items;
    QJsonObject m_answer;

    QLabel *m_title = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_hints = nullptr;
    QPushButton *m_confirmButton = nullptr;
};
