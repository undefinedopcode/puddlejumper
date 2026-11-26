/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "palette.h"

#include "appicon.h"
#include "cardstyle.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kShadow = 14;  // room around the panel for its shadow
constexpr int kRadius = 12;  // the panel's corners

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
        const bool two = !index.data(PaletteDialog::SubtitleRole).toString().isEmpty();
        return QSize(0, two ? fm.height() * 2 + 16 : fm.height() + 16);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        using R = PaletteDialog;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = m_palette;  // the palette's own, not the view's
        const QColor text = pal.color(QPalette::WindowText);
        const QColor quiet = muted(pal);
        const QColor accent = cardstyle::accent();
        const QRect row = option.rect.adjusted(6, 1, -6, -1);
        const int kind = index.data(R::KindRole).toInt();

        if (kind == R::EmptyRow) {
            p->setPen(quiet);
            p->drawText(row, Qt::AlignCenter, index.data(R::TitleRole).toString());
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
        if (m_ticks && kind == R::ItemRow) {
            const QRectF circle(x, mid - 8, 16, 16);
            if (index.data(R::TickedRole).toBool()) {
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
        QString icon = index.data(R::IconRole).toString();
        if (icon.isEmpty() && kind == R::UseRow)
            icon = QStringLiteral("pencil");
        if (!icon.isEmpty()) {
            const QIcon drawn = appIcon(icon.toLatin1().constData());
            if (!drawn.isNull()) {
                drawn.paint(p, QRect(x, mid - 9, 18, 18));
                x += 28;
            }
        }

        // The detail (a status, or keys), in a pill at the far end.
        int right = row.right() - 10;
        const QString detail = index.data(R::DetailRole).toString();
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
        const QString subtitle = index.data(R::SubtitleRole).toString();
        const int titleTop = subtitle.isEmpty() ? mid - fm.height() / 2 : mid - fm.height() + 1;
        const QRect titleRect(x, titleTop, right - x, fm.height());
        const int used = paintTitle(p, option.font, titleRect, index.data(R::TitleRole).toString(),
                                    index.data(R::MatchRole).toStringList(), kind == R::UseRow, text);
        // A note, quieter, after the title on its line.
        const QString note = index.data(R::NoteRole).toString();
        if (!note.isEmpty() && used + 24 < titleRect.width()) {
            const QRect noteRect(x + used + 12, titleTop, titleRect.width() - used - 12, fm.height());
            p->setFont(option.font);
            p->setPen(quiet);
            p->drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(note, Qt::ElideRight, noteRect.width()));
        }
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
    // The title, the filter's words (or a Use row's quoted text) in bold;
    // gives the width drawn.
    static int paintTitle(QPainter *p, const QFont &font, const QRect &rect, const QString &title,
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
        return int(std::ceil(line.naturalTextWidth()));
    }

    bool m_ticks;
    QPalette m_palette;
};

}  // namespace

PaletteDialog::PaletteDialog(const Options &options, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    , m_maxRows(std::max(1, options.maxRows))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadow + 8, kShadow + (options.heading.isEmpty() ? 6 : 10), kShadow + 8, kShadow + 8);
    outer->setSpacing(6);

    QPalette quietPal = palette();
    quietPal.setColor(QPalette::WindowText, muted(palette()));

    m_heading = new QLabel(options.heading, this);
    QFont headingFont = m_heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 0.92);
    headingFont.setWeight(QFont::DemiBold);
    m_heading->setFont(headingFont);
    m_heading->setPalette(quietPal);
    m_heading->setContentsMargins(10, 0, 10, 0);
    m_heading->setVisible(!options.heading.isEmpty());
    outer->addWidget(m_heading);

    auto *fieldRow = new QHBoxLayout;
    fieldRow->setContentsMargins(10, 0, 10, 0);
    fieldRow->setSpacing(8);
    if (options.searchIcon) {
        auto *glass = new QLabel(this);
        glass->setPixmap(appIcon("search").pixmap(18, 18));
        fieldRow->addWidget(glass);
    }
    m_filter = new QLineEdit(this);
    m_filter->setFrame(false);
    m_filter->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; padding: 4px 0; }"));
    QFont fieldFont = m_filter->font();
    fieldFont.setPointSizeF(fieldFont.pointSizeF() * 1.25);
    m_filter->setFont(fieldFont);
    m_filter->setPlaceholderText(options.placeholder);
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
    m_list->setItemDelegate(new RowDelegate(options.ticks, palette(), m_list));
    outer->addWidget(m_list, 1);

    m_footer = new QHBoxLayout;
    m_footer->setContentsMargins(10, 2, 4, 0);
    m_hints = new QLabel(this);
    QFont hintFont = m_hints->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * 0.85);
    m_hints->setFont(hintFont);
    m_hints->setPalette(quietPal);
    m_footer->addWidget(m_hints, 1);
    outer->addLayout(m_footer);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *row) { activate(row, false); });
    m_filter->installEventFilter(this);
    m_filter->setFocus();
}

QListWidgetItem *PaletteDialog::addRow(RowKind kind, const QString &title)
{
    auto *row = new QListWidgetItem(m_list);
    row->setData(KindRole, kind);
    row->setData(TitleRole, title);
    return row;
}

void PaletteDialog::addEmptyRow(const QString &text)
{
    QListWidgetItem *row = addRow(EmptyRow, text);
    row->setFlags(Qt::NoItemFlags);
}

void PaletteDialog::fitRows()
{
    int height = 0;
    for (int i = 0; i < m_list->count() && i < m_maxRows; ++i)
        height += m_list->sizeHintForRow(i);
    m_list->setFixedHeight(std::max(height, 40) + 4);
}

void PaletteDialog::setHints(const QString &text)
{
    m_hints->setText(text);
}

QStringList PaletteDialog::words(const QString &typed)
{
    return typed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool PaletteDialog::matches(const QStringList &words, const QString &haystack)
{
    for (const QString &w : words)
        if (!haystack.contains(w, Qt::CaseInsensitive))
            return false;
    return true;
}

QString PaletteDialog::confirmKeys()
{
#ifdef Q_OS_MACOS
    return QStringLiteral("⌘↵");
#else
    return QStringLiteral("Ctrl+↵");
#endif
}

void PaletteDialog::step(int by)
{
    const int n = m_list->count();
    if (n == 0 || !(m_list->item(0)->flags() & Qt::ItemIsSelectable))
        return;
    m_list->setCurrentRow(std::clamp(m_list->currentRow() + by, 0, n - 1));
    m_list->scrollToItem(m_list->currentItem());
}

bool PaletteDialog::eventFilter(QObject *watched, QEvent *event)
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
        const int page = std::max(1, m_maxRows - 1);
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

void PaletteDialog::paintEvent(QPaintEvent *)
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

void PaletteDialog::showEvent(QShowEvent *event)
{
    // Across the top of the window.
    QWidget *over = parentWidget() ? parentWidget()->window() : nullptr;
    const QRect area = over ? over->geometry()
                            : (screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800));
    const int width = std::clamp(int(area.width() * 0.6), 460, 680) + 2 * kShadow;
    resize(width, sizeHint().height());
    move(area.center().x() - width / 2, area.top() + std::max(40, int(area.height() * 0.12)) - kShadow);
    QDialog::showEvent(event);
}

void PaletteDialog::setFilter(const QString &text)
{
    m_filter->setText(text);
}

QStringList PaletteDialog::shownTitles() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        out << m_list->item(i)->data(TitleRole).toString();
    return out;
}

QString PaletteDialog::currentTitle() const
{
    const QListWidgetItem *row = m_list->currentItem();
    return row ? row->data(TitleRole).toString() : QString();
}
