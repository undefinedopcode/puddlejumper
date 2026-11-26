/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexprompt.h"

#include "appicon.h"
#include "cardstyle.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kShadow = 14;     // room around the panel for its shadow
constexpr int kRadius = 12;     // the panel's corners
constexpr int kMaxRows = 8;     // rows shown before the list scrolls

enum Role {
    KindRole = Qt::UserRole,  // RowKind
    IndexRole,                // into m_items
    TitleRole,
    SubtitleRole,
    IconRole,
    DetailRole,
    TickedRole,
    MatchRole,  // the filter's words, to embolden in the title
};
enum RowKind { ItemRow, UseRow, EmptyRow };

// Rex's icon names, drawn with the app's Lucide icons.
const QHash<QString, const char *> &iconMap()
{
    static const QHash<QString, const char *> map{
        {QStringLiteral("document"), "file-text"}, {QStringLiteral("folder"), "folder"},
        {QStringLiteral("branch"), "git-branch"},  {QStringLiteral("terminal"), "square-terminal"},
        {QStringLiteral("code"), "code-xml"},      {QStringLiteral("server"), "server"},
        {QStringLiteral("database"), "database"},  {QStringLiteral("package"), "package"},
        {QStringLiteral("globe"), "globe"},        {QStringLiteral("link"), "link"},
        {QStringLiteral("person"), "user"},        {QStringLiteral("tag"), "tag"},
        {QStringLiteral("clock"), "clock"},        {QStringLiteral("star"), "star"},
        {QStringLiteral("gear"), "settings"},      {QStringLiteral("plus"), "plus"},
    };
    return map;
}

QColor muted(const QPalette &pal)
{
    return cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.58);
}

// The panel's colour: the window's, a touch towards the text, so it stands
// off whatever it is over.
QColor panelColour(const QPalette &pal)
{
    return cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.03);
}

/*
 * A row: a tick circle (multiple), the icon, the title (the words typed in
 * bold) over its subtitle, and the detail in a pill at the far end.
 */
class RowDelegate : public QStyledItemDelegate
{
public:
    RowDelegate(bool ticks, const QPalette &palette, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_ticks(ticks)
        , m_palette(palette)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QFontMetrics fm(option.font);
        const bool two = !index.data(SubtitleRole).toString().isEmpty();
        return QSize(0, two ? fm.height() * 2 + 16 : fm.height() + 16);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = m_palette;  // the prompt's, not the view's
        const QColor text = pal.color(QPalette::WindowText);
        const QColor quiet = muted(pal);
        const QColor accent = cardstyle::accent();
        const QRect row = option.rect.adjusted(6, 1, -6, -1);
        const int kind = index.data(KindRole).toInt();

        if (kind == EmptyRow) {
            p->setPen(quiet);
            p->drawText(row, Qt::AlignCenter, index.data(TitleRole).toString());
            p->restore();
            return;
        }
        const bool current = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (current || hovered) {
            QPainterPath path;
            path.addRoundedRect(QRectF(row), 7, 7);
            p->fillPath(path, cardstyle::mix(panelColour(pal), accent, current ? 0.22 : 0.08));
        }

        int x = row.left() + 10;
        const int mid = row.center().y();
        if (m_ticks && kind == ItemRow) {
            const QRectF circle(x, mid - 8, 16, 16);
            if (index.data(TickedRole).toBool()) {
                p->setPen(Qt::NoPen);
                p->setBrush(accent);
                p->drawEllipse(circle);
                QPainterPath check;
                check.moveTo(circle.left() + 4.2, circle.center().y() + 0.2);
                check.lineTo(circle.left() + 7, circle.top() + 11);
                check.lineTo(circle.right() - 3.8, circle.top() + 5);
                p->setPen(QPen(pal.color(QPalette::HighlightedText), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->setBrush(Qt::NoBrush);
                p->drawPath(check);
            } else {
                p->setPen(QPen(cardstyle::mix(panelColour(pal), text, 0.35), 1.3));
                p->setBrush(Qt::NoBrush);
                p->drawEllipse(circle.adjusted(0.65, 0.65, -0.65, -0.65));
            }
            x += 26;
        }
        const QString iconName = index.data(IconRole).toString();
        if (const char *lucide = iconMap().value(iconName, nullptr)) {
            appIcon(lucide).paint(p, QRect(x, mid - 9, 18, 18));
            x += 28;
        } else if (kind == UseRow) {
            appIcon("pencil").paint(p, QRect(x, mid - 8, 16, 16));
            x += 28;
        }

        // The detail, in a pill at the far end.
        int right = row.right() - 10;
        const QString detail = index.data(DetailRole).toString();
        if (!detail.isEmpty()) {
            QFont small = option.font;
            small.setPointSizeF(small.pointSizeF() * 0.85);
            const QFontMetrics sfm(small);
            const QString shown = sfm.elidedText(detail, Qt::ElideRight, row.width() / 3);
            const int w = sfm.horizontalAdvance(shown) + 14;
            const QRect pill(right - w, mid - sfm.height() / 2 - 3, w, sfm.height() + 6);
            QPainterPath path;
            path.addRoundedRect(QRectF(pill), pill.height() / 2.0, pill.height() / 2.0);
            p->fillPath(path, cardstyle::mix(panelColour(pal), text, 0.08));
            p->setFont(small);
            p->setPen(quiet);
            p->drawText(pill, Qt::AlignCenter, shown);
            right = pill.left() - 10;
        }

        const QFontMetrics fm(option.font);
        const QString subtitle = index.data(SubtitleRole).toString();
        const int titleTop = subtitle.isEmpty() ? mid - fm.height() / 2 : mid - fm.height() + 1;
        const QRect titleRect(x, titleTop, right - x, fm.height());
        paintTitle(p, option.font, titleRect, index.data(TitleRole).toString(), index.data(MatchRole).toStringList(),
                   kind == UseRow, text);
        if (!subtitle.isEmpty()) {
            QFont sub = option.font;
            sub.setPointSizeF(sub.pointSizeF() * 0.9);
            p->setFont(sub);
            p->setPen(quiet);
            const QFontMetrics sfm(sub);
            p->drawText(QRect(x, mid + 2, right - x, sfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        sfm.elidedText(subtitle, Qt::ElideMiddle, right - x));
        }
        p->restore();
    }

private:
    // The title, the filter's words (or a Use row's quoted text) in bold.
    static void paintTitle(QPainter *p, const QFont &font, const QRect &rect, const QString &title,
                           const QStringList &words, bool useRow, const QColor &colour)
    {
        const QString shown = QFontMetrics(font).elidedText(title, Qt::ElideRight, rect.width());
        QList<QTextLayout::FormatRange> ranges;
        QTextCharFormat bold;
        bold.setFontWeight(QFont::DemiBold);
        if (useRow) {
            const int open = shown.indexOf(QChar(0x201c));
            if (open >= 0)
                ranges << QTextLayout::FormatRange{open, int(shown.size()) - open, bold};
        } else {
            for (const QString &w : words) {
                for (int at = shown.indexOf(w, 0, Qt::CaseInsensitive); at >= 0;
                     at = shown.indexOf(w, at + w.size(), Qt::CaseInsensitive))
                    ranges << QTextLayout::FormatRange{at, int(w.size()), bold};
            }
        }
        QTextLayout layout(shown, font);
        layout.setFormats(ranges);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        line.setLineWidth(rect.width() + 20);  // elided already
        layout.endLayout();
        p->setPen(colour);
        layout.draw(p, rect.topLeft());
    }

    bool m_ticks;
    QPalette m_palette;
};

QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

}  // namespace

QStringList RexPrompt::iconNames()
{
    QStringList out = iconMap().keys();
    out.sort();
    return out;
}

RexPrompt::RexPrompt(const QJsonObject &prompt, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    m_pick = text(prompt, "kind") == QLatin1String("pick");
    m_multiple = m_pick && prompt.value(QLatin1String("multiple")).toBool();
    m_allowOther = m_pick && !m_multiple && prompt.value(QLatin1String("allow_other")).toBool();
    m_confirm = text(prompt, "confirm");
    const QString title = text(prompt, "title");
    setWindowTitle(title);

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
            it.icon = text(o, "icon");
            it.detail = text(o, "detail");
            it.ticked = m_multiple && o.value(QLatin1String("selected")).toBool();
        }
        m_items << it;
    }

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadow + 8, kShadow + 10, kShadow + 8, kShadow + 8);
    outer->setSpacing(6);

    QPalette quietPal = palette();
    quietPal.setColor(QPalette::WindowText, muted(palette()));

    m_title = new QLabel(title, this);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.92);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setPalette(quietPal);
    m_title->setContentsMargins(10, 0, 10, 0);
    outer->addWidget(m_title);

    auto *fieldRow = new QHBoxLayout;
    fieldRow->setContentsMargins(10, 0, 10, 0);
    fieldRow->setSpacing(8);
    if (m_pick) {
        auto *glass = new QLabel(this);
        glass->setPixmap(appIcon("search").pixmap(18, 18));
        glass->setPalette(quietPal);
        fieldRow->addWidget(glass);
    }
    m_filter = new QLineEdit(this);
    m_filter->setFrame(false);
    m_filter->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 4px 0; }"));
    QFont fieldFont = m_filter->font();
    fieldFont.setPointSizeF(fieldFont.pointSizeF() * 1.25);
    m_filter->setFont(fieldFont);
    m_filter->setPlaceholderText(text(prompt, "placeholder"));
    if (m_filter->placeholderText().isEmpty() && m_pick)
        m_filter->setPlaceholderText(tr("Filter"));
    m_filter->setText(text(prompt, "value"));
    fieldRow->addWidget(m_filter, 1);
    outer->addLayout(fieldRow);

    auto *rule = new QFrame(this);
    rule->setFixedHeight(1);
    rule->setAutoFillBackground(true);
    QPalette rulePal = palette();
    rulePal.setColor(QPalette::Window, cardstyle::mix(panelColour(palette()), palette().color(QPalette::WindowText), 0.12));
    rule->setPalette(rulePal);
    outer->addWidget(rule);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    m_list->setFocusPolicy(Qt::NoFocus);  // the field keeps the keys
    // Transparent over the panel; rows are drawn by the delegate alone.
    QPalette listPal = palette();
    listPal.setColor(QPalette::Base, Qt::transparent);
    m_list->setPalette(listPal);
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setItemDelegate(new RowDelegate(m_multiple, palette(), m_list));
    outer->addWidget(m_list, 1);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(10, 2, 4, 0);
    m_hints = new QLabel(this);
    QFont hintFont = m_hints->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * 0.85);
    m_hints->setFont(hintFont);
    m_hints->setPalette(quietPal);
    footer->addWidget(m_hints, 1);
    if (m_multiple) {
        m_confirmButton = new QPushButton(this);
        m_confirmButton->setFocusPolicy(Qt::NoFocus);
        connect(m_confirmButton, &QPushButton::clicked, this, [this]() { activate(nullptr, true); });
        footer->addWidget(m_confirmButton);
    }
    outer->addLayout(footer);

    const QString enter = QStringLiteral("↵");
    if (!m_pick)
        m_hints->setText(tr("%1 Submit    Esc Cancel").arg(enter));
    else if (m_multiple)
        m_hints->setText(tr("↑↓ Move    %1 Tick    %2 %3    Esc Cancel")
                             .arg(enter, confirmKeys(), m_confirm.isEmpty() ? tr("Done") : m_confirm));
    else
        m_hints->setText(tr("↑↓ Move    %1 Choose    Esc Cancel").arg(enter));

    connect(m_filter, &QLineEdit::textChanged, this, &RexPrompt::refilter);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *row) { activate(row, false); });
    m_filter->installEventFilter(this);
    refilter();
    updateConfirm();

    // A height for kMaxRows (or fewer, when there are fewer), kept while
    // filtering so the palette doesn't jump.
    int rows = 0;
    for (int i = 0; i < m_list->count() && i < kMaxRows; ++i)
        rows += m_list->sizeHintForRow(i);
    if (m_pick && m_items.isEmpty() && !m_allowOther)
        rows = m_list->sizeHintForRow(0);
    m_list->setFixedHeight(std::max(rows, m_list->count() ? 0 : 40) + 4);
    m_filter->setFocus();
}

QString RexPrompt::confirmKeys() const
{
#ifdef Q_OS_MACOS
    return QStringLiteral("⌘↵");
#else
    return QStringLiteral("Ctrl+↵");
#endif
}

void RexPrompt::refilter()
{
    const QString typed = m_filter->text();
    const QStringList words = typed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int keep = m_list->currentRow();
    m_list->clear();
    const auto useRow = [this, &typed](bool current) {
        auto *row = new QListWidgetItem(m_list);
        row->setData(KindRole, UseRow);
        // Typographic quotes, so the quoted text reads (and is emboldened).
        const QString use = tr("Use “%1”").arg(typed);
        if (!m_pick && !m_confirm.isEmpty()) {
            row->setData(TitleRole, m_confirm);
            if (!typed.isEmpty())
                row->setData(SubtitleRole, typed);
        } else {
            row->setData(TitleRole, use);
        }
        if (current)
            m_list->setCurrentItem(row);
    };
    if (!m_pick) {
        useRow(true);
        return;
    }
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &it = m_items.at(i);
        const QString hay = it.title + QLatin1Char(' ') + it.subtitle + QLatin1Char(' ') + it.id + QLatin1Char(' ')
                            + it.detail;
        bool all = true;
        for (const QString &w : words)
            all = all && hay.contains(w, Qt::CaseInsensitive);
        if (!all)
            continue;
        auto *row = new QListWidgetItem(m_list);
        row->setData(KindRole, ItemRow);
        row->setData(IndexRole, i);
        row->setData(TitleRole, it.title);
        row->setData(SubtitleRole, it.subtitle);
        row->setData(IconRole, it.icon);
        row->setData(DetailRole, it.detail);
        row->setData(TickedRole, it.ticked);
        row->setData(MatchRole, words);
        row->setToolTip(it.id == it.title ? QString() : it.id);
    }
    const int matches = m_list->count();
    if (m_allowOther && !typed.trimmed().isEmpty())
        useRow(matches == 0);
    if (m_list->count() == 0) {
        auto *row = new QListWidgetItem(m_list);
        row->setData(KindRole, EmptyRow);
        row->setData(TitleRole, m_items.isEmpty() ? tr("Nothing to choose from") : tr("No matches"));
        row->setFlags(Qt::NoItemFlags);
        return;
    }
    if (!m_list->currentItem())
        m_list->setCurrentRow(words.isEmpty() && keep >= 0 && keep < m_list->count() ? keep : 0);
}

void RexPrompt::step(int by)
{
    const int n = m_list->count();
    if (n == 0 || !(m_list->item(0)->flags() & Qt::ItemIsSelectable))
        return;
    m_list->setCurrentRow(std::clamp(m_list->currentRow() + by, 0, n - 1));
    m_list->scrollToItem(m_list->currentItem());
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
        finish(QJsonObject{{QStringLiteral("text"), m_filter->text()}});
        return;
    }
    if (!row)
        return;
    switch (row->data(KindRole).toInt()) {
    case ItemRow:
        finish(QJsonObject{
            {QStringLiteral("ids"), QJsonArray{m_items.at(row->data(IndexRole).toInt()).id}}});
        break;
    case UseRow:
        finish(QJsonObject{{QStringLiteral("text"), m_filter->text()}});
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

bool RexPrompt::eventFilter(QObject *watched, QEvent *event)
{
    // A starting value is to be added to, not replaced: the dialog's first
    // focus (a Tab reason) would select it all.
    if (watched == m_filter && event->type() == QEvent::FocusIn) {
        QTimer::singleShot(0, m_filter, [field = m_filter]() {
            field->deselect();
            field->end(false);
        });
    }
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int page = std::max(1, kMaxRows - 1);
        switch (key->key()) {
        case Qt::Key_Down: step(1); return true;
        case Qt::Key_Up: step(-1); return true;
        case Qt::Key_PageDown: step(page); return true;
        case Qt::Key_PageUp: step(-page); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            // Cmd+Return (Qt's Control on macOS), or Ctrl+Return elsewhere.
            const bool confirm = key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
            activate(m_list->currentItem(), confirm);
            return true;
        }
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void RexPrompt::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF panel = QRectF(rect()).adjusted(kShadow, kShadow, -kShadow, -kShadow);
    // A soft shadow: rings fading out from the panel.
    for (int i = kShadow; i > 0; --i) {
        QPainterPath ring;
        ring.addRoundedRect(panel.adjusted(-i, -i + 4, i, i + 4).intersected(QRectF(rect())), kRadius + i, kRadius + i);
        const qreal t = 1.0 - qreal(i) / kShadow;
        p.fillPath(ring, QColor(0, 0, 0, int(6 * t * t)));
    }
    QPainterPath path;
    path.addRoundedRect(panel, kRadius, kRadius);
    p.fillPath(path, panelColour(palette()));
    p.setPen(QPen(cardstyle::mix(palette().color(QPalette::Window), palette().color(QPalette::WindowText), 0.16), 1));
    p.drawPath(path);
}

void RexPrompt::showEvent(QShowEvent *event)
{
    // Across the top of the window, as a palette.
    QWidget *over = parentWidget() ? parentWidget()->window() : nullptr;
    const QRect area = over ? over->geometry()
                            : (screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800));
    const int width = std::clamp(int(area.width() * 0.6), 460, 680) + 2 * kShadow;
    resize(width, sizeHint().height());
    move(area.center().x() - width / 2, area.top() + std::max(40, int(area.height() * 0.12)) - kShadow);
    QDialog::showEvent(event);
}

void RexPrompt::setFilter(const QString &text)
{
    m_filter->setText(text);
}

QStringList RexPrompt::shownTitles() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        out << m_list->item(i)->data(TitleRole).toString();
    return out;
}

QString RexPrompt::currentTitle() const
{
    const QListWidgetItem *row = m_list->currentItem();
    return row ? row->data(TitleRole).toString() : QString();
}

QStringList RexPrompt::tickedIds() const
{
    QStringList out;
    for (const Item &it : m_items)
        if (it.ticked)
            out << it.id;
    return out;
}
