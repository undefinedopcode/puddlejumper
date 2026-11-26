/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexprompt.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

namespace {

// Rex's icon names, drawn with the app's Lucide icons.
const QHash<QString, QString> &iconMap()
{
    static const QHash<QString, QString> map{
        {QStringLiteral("document"), QStringLiteral("file-text")},
        {QStringLiteral("folder"), QStringLiteral("folder")},
        {QStringLiteral("branch"), QStringLiteral("git-branch")},
        {QStringLiteral("terminal"), QStringLiteral("square-terminal")},
        {QStringLiteral("code"), QStringLiteral("code-xml")},
        {QStringLiteral("server"), QStringLiteral("server")},
        {QStringLiteral("database"), QStringLiteral("database")},
        {QStringLiteral("package"), QStringLiteral("package")},
        {QStringLiteral("globe"), QStringLiteral("globe")},
        {QStringLiteral("link"), QStringLiteral("link")},
        {QStringLiteral("person"), QStringLiteral("user")},
        {QStringLiteral("tag"), QStringLiteral("tag")},
        {QStringLiteral("clock"), QStringLiteral("clock")},
        {QStringLiteral("star"), QStringLiteral("star")},
        {QStringLiteral("gear"), QStringLiteral("settings")},
        {QStringLiteral("plus"), QStringLiteral("plus")},
    };
    return map;
}

QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

PaletteDialog::Options optionsFor(const QJsonObject &prompt)
{
    PaletteDialog::Options o;
    const bool pick = text(prompt, "kind") == QLatin1String("pick");
    o.heading = text(prompt, "title");
    o.placeholder = text(prompt, "placeholder");
    if (o.placeholder.isEmpty() && pick)
        o.placeholder = QObject::tr("Filter");
    o.searchIcon = pick;
    o.ticks = pick && prompt.value(QLatin1String("multiple")).toBool();
    return o;
}

}  // namespace

QStringList RexPrompt::iconNames()
{
    QStringList out = iconMap().keys();
    out.sort();
    return out;
}

RexPrompt::RexPrompt(const QJsonObject &prompt, QWidget *parent)
    : PaletteDialog(optionsFor(prompt), parent)
{
    m_pick = text(prompt, "kind") == QLatin1String("pick");
    m_multiple = m_pick && prompt.value(QLatin1String("multiple")).toBool();
    m_allowOther = m_pick && !m_multiple && prompt.value(QLatin1String("allow_other")).toBool();
    m_confirm = text(prompt, "confirm");
    setWindowTitle(text(prompt, "title"));

    for (const QJsonValue &v : prompt.value(QLatin1String("items")).toArray()) {
        Item it;
        if (v.isString()) {
            it.id = it.title = v.toString();
        } else {
            const QJsonObject o = v.toObject();
            it.id = o.value(QLatin1String("id")).toVariant().toString();
            it.title = text(o, "title");
            if (it.title.isEmpty())
                it.title = it.id;
            it.subtitle = text(o, "subtitle");
            it.icon = iconMap().value(text(o, "icon"));
            it.detail = text(o, "detail");
            it.ticked = m_multiple && o.value(QLatin1String("selected")).toBool();
        }
        m_items << it;
    }

    if (m_multiple) {
        m_confirmButton = new QPushButton(this);
        m_confirmButton->setFocusPolicy(Qt::NoFocus);
        connect(m_confirmButton, &QPushButton::clicked, this, [this]() { activate(nullptr, true); });
        footer()->addWidget(m_confirmButton);
    }
    const QString enter = QStringLiteral("↵");
    if (!m_pick)
        setHints(tr("%1 Submit    Esc Cancel").arg(enter));
    else if (m_multiple)
        setHints(tr("↑↓ Move    %1 Tick    %2 %3    Esc Cancel")
                     .arg(enter, confirmKeys(), m_confirm.isEmpty() ? tr("Done") : m_confirm));
    else
        setHints(tr("↑↓ Move    %1 Choose    Esc Cancel").arg(enter));

    filterField()->setText(text(prompt, "value"));
    connect(filterField(), &QLineEdit::textChanged, this, &RexPrompt::refilter);
    refilter();
    updateConfirm();
    fitRows();
}

void RexPrompt::refilter()
{
    QListWidget *rows = list();
    const QString typed = filterField()->text();
    const QStringList typedWords = words(typed);
    const int keep = rows->currentRow();
    rows->clear();
    const auto useRow = [this, rows, &typed](bool current) {
        // Typographic quotes, so the quoted text reads (and is emboldened).
        QListWidgetItem *row = addRow(UseRow, tr("Use “%1”").arg(typed));
        if (!m_pick && !m_confirm.isEmpty()) {
            row->setData(TitleRole, m_confirm);
            if (!typed.isEmpty())
                row->setData(SubtitleRole, typed);
        }
        if (current)
            rows->setCurrentItem(row);
    };
    if (!m_pick) {
        useRow(true);
        return;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items.at(i);
        if (!matches(typedWords, it.title + QLatin1Char(' ') + it.subtitle + QLatin1Char(' ') + it.id
                                     + QLatin1Char(' ') + it.detail))
            continue;
        QListWidgetItem *row = addRow(ItemRow, it.title);
        row->setData(IndexRole, i);
        row->setData(SubtitleRole, it.subtitle);
        row->setData(IconRole, it.icon);
        row->setData(DetailRole, it.detail);
        row->setData(TickedRole, it.ticked);
        row->setData(MatchRole, typedWords);
        row->setToolTip(it.id == it.title ? QString() : it.id);
    }
    const int found = rows->count();
    if (m_allowOther && !typed.trimmed().isEmpty())
        useRow(found == 0);
    if (rows->count() == 0) {
        addEmptyRow(m_items.isEmpty() ? tr("Nothing to choose from") : tr("No matches"));
        return;
    }
    if (!rows->currentItem())
        rows->setCurrentRow(typedWords.isEmpty() && keep >= 0 && keep < rows->count() ? keep : 0);
}

void RexPrompt::activate(QListWidgetItem *row, bool confirm)
{
    if (m_multiple) {
        if (!confirm) {
            if (!row || row->data(KindRole).toInt() != ItemRow)
                return;
            Item &it = m_items[row->data(IndexRole).toInt()];
            it.ticked = !it.ticked;
            row->setData(TickedRole, it.ticked);
            updateConfirm();
            return;
        }
        QJsonArray ids;  // in the items' order
        for (const Item &it : std::as_const(m_items))
            if (it.ticked)
                ids << it.id;
        finish(QJsonObject{{QStringLiteral("ids"), ids}});
        return;
    }
    if (!m_pick) {
        finish(QJsonObject{{QStringLiteral("text"), filterField()->text()}});
        return;
    }
    if (!row)
        return;
    switch (row->data(KindRole).toInt()) {
    case ItemRow:
        finish(QJsonObject{{QStringLiteral("ids"), QJsonArray{m_items.at(row->data(IndexRole).toInt()).id}}});
        break;
    case UseRow:
        finish(QJsonObject{{QStringLiteral("text"), filterField()->text()}});
        break;
    default:
        break;
    }
}

void RexPrompt::updateConfirm()
{
    if (!m_confirmButton)
        return;
    int n = 0;
    for (const Item &it : std::as_const(m_items))
        n += it.ticked;
    const QString label = m_confirm.isEmpty() ? tr("Done") : m_confirm;
    m_confirmButton->setText(n ? QStringLiteral("%1  ·  %2").arg(label).arg(n) : label);
}

void RexPrompt::finish(const QJsonObject &answer)
{
    m_answer = answer;
    accept();
}

QStringList RexPrompt::tickedIds() const
{
    QStringList out;
    for (const Item &it : m_items)
        if (it.ticked)
            out << it.id;
    return out;
}
