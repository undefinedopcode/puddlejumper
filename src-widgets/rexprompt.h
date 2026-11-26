/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "palette.h"

#include <QJsonObject>
#include <QStringList>

class QPushButton;

/*
 * A Rex server's question (client_prompt_requested: an action's
 * rex.ui.input or rex.ui.pick), asked as Rex's apps ask it: a palette over
 * the window (palette.h), its title above the text field.
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
class RexPrompt : public PaletteDialog
{
    Q_OBJECT

public:
    explicit RexPrompt(const QJsonObject &prompt, QWidget *parent = nullptr);

    QJsonObject answer() const { return m_answer; }

    /* The icon names Rex's prompts take ("folder", "branch", …). */
    static QStringList iconNames();

    /* For tests: the ids ticked. */
    QStringList tickedIds() const;

protected:
    void activate(QListWidgetItem *row, bool confirm) override;

private:
    struct Item
    {
        QString id, title, subtitle, icon, detail;
        bool ticked = false;
    };
    void refilter();
    void updateConfirm();
    void finish(const QJsonObject &answer);

    bool m_pick = false;
    bool m_multiple = false;
    bool m_allowOther = false;
    QString m_confirm;
    QList<Item> m_items;
    QJsonObject m_answer;
    QPushButton *m_confirmButton = nullptr;
};
