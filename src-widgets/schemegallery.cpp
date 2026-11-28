#include "schemegallery.h"

#include "cardstyle.h"
#include "colorscheme.h"

#include <QEvent>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyledItemDelegate>

namespace {

constexpr int kCardWidth = 168;
constexpr int kCardHeight = 92;
constexpr int kGap = 10;
constexpr int kRadius = 8;

enum { ColoursRole = Qt::UserRole + 1 };

/* A card: the scheme's own background with a prompt line and its swatches,
 * the name under it. */
class CardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(kCardWidth, kCardHeight + option.fontMetrics.height() + 8);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QVector<QColor> colours = index.data(ColoursRole).value<QVector<QColor>>();
        if (colours.size() < ColorSchemeFile::RoleCount)
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = option.palette;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;

        const QRectF card = QRectF(option.rect.left() + 2, option.rect.top() + 2, kCardWidth - 4, kCardHeight - 4);
        const QColor bg = colours.at(ColorSchemeFile::Background);
        const QColor fg = colours.at(ColorSchemeFile::Foreground);
        QPainterPath path;
        path.addRoundedRect(card, kRadius, kRadius);
        p->fillPath(path, bg);

        // A prompt and a command, then a line of output in two colours.
        QFont mono(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(11);
        p->setFont(mono);
        const QFontMetrics fm(mono);
        const qreal x = card.left() + 10;
        qreal y = card.top() + 8 + fm.ascent();
        const auto run = [&](qreal at, const QString &text, const QColor &c) {
            p->setPen(c);
            p->drawText(QPointF(at, y), text);
            return at + fm.horizontalAdvance(text);
        };
        qreal at = run(x, QStringLiteral("~ "), colours.at(ColorSchemeFile::Color4Intense));
        at = run(at, QStringLiteral("$ "), colours.at(ColorSchemeFile::Color2));
        run(at, QStringLiteral("ls -l"), fg);
        y += fm.height();
        at = run(x, QStringLiteral("src/ "), colours.at(ColorSchemeFile::Color4));
        at = run(at, QStringLiteral("run.sh "), colours.at(ColorSchemeFile::Color2Intense));
        run(at, QStringLiteral("notes"), fg);

        // The sixteen colours: normal over bright.
        const qreal sw = (card.width() - 20) / 8.0;
        const qreal sh = 9;
        const qreal top = card.bottom() - 10 - 2 * sh - 2;
        for (int row = 0; row < 2; ++row) {
            for (int i = 0; i < 8; ++i) {
                const int role = (row == 0 ? ColorSchemeFile::Color0 : ColorSchemeFile::Color0Intense) + i;
                const QRectF r(x + i * sw, top + row * (sh + 2), sw - 2, sh);
                p->setPen(Qt::NoPen);
                p->setBrush(colours.at(role));
                p->drawRoundedRect(r, 2, 2);
            }
        }

        // Its edge: the accent when chosen, a faint line otherwise (so a
        // background like the window's still reads as a card).
        const QColor accent = pal.color(QPalette::Highlight);
        if (selected) {
            p->setPen(QPen(accent, 2.5));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(card.adjusted(-1, -1, 1, 1), kRadius + 1, kRadius + 1);
        } else {
            const QColor line = cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText),
                                               hovered ? 0.45 : 0.18);
            p->setPen(QPen(line, 1));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
        }

        // The name, under it.
        QFont nameFont = option.font;
        if (selected)
            nameFont.setWeight(QFont::DemiBold);
        p->setFont(nameFont);
        p->setPen(selected ? pal.color(QPalette::WindowText)
                           : cardstyle::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.8));
        const QRect nameRect(option.rect.left() + 2, int(card.bottom()) + 4, kCardWidth - 4,
                             option.fontMetrics.height() + 2);
        p->drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop,
                    QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                                      nameRect.width()));
        p->restore();
    }
};

QVector<QColor> coloursOf(const QString &name, const QPalette &palette)
{
    ColorSchemeFile scheme = name == ColorSchemeFile::systemName() ? ColorSchemeFile::systemScheme(palette)
                                                                    : ColorSchemeFile::defaultScheme();
    if (name != ColorSchemeFile::systemName()) {
        const QString file = ColorSchemeFile::findSchemeFile(name);
        if (file.isEmpty() || !scheme.load(file))
            return {};
    }
    return scheme.colors;
}

} // namespace

SchemeGallery::SchemeGallery(QWidget *parent)
    : QListView(parent)
    , m_model(new QStandardItemModel(this))
{
    setModel(m_model);
    setItemDelegate(new CardDelegate(this));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSpacing(kGap / 2);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(24);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_Hover);

    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) {
        if (!m_quiet && index.isValid())
            emit chosen(index.data(Qt::DisplayRole).toString());
    });
}

void SchemeGallery::setSchemes(const QStringList &names)
{
    const QString keep = current();
    m_quiet = true;
    m_model->clear();
    m_names.clear();
    for (const QString &name : names) {
        const QVector<QColor> colours = coloursOf(name, palette());
        if (colours.size() < ColorSchemeFile::RoleCount)
            continue;   // a file that doesn't read: not worth a broken card
        auto *item = new QStandardItem(name);
        item->setData(QVariant::fromValue(colours), ColoursRole);
        item->setToolTip(name);
        item->setEditable(false);
        m_model->appendRow(item);
        m_names << name;
    }
    m_quiet = false;
    setCurrent(keep);
}

void SchemeGallery::setCurrent(const QString &name)
{
    QString target = name;
    if (target.contains(QLatin1Char('/')))
        target = QFileInfo(target).baseName();
    m_quiet = true;
    const int row = int(m_names.indexOf(target));
    if (row >= 0) {
        const QModelIndex index = m_model->index(row, 0);
        setCurrentIndex(index);
        scrollTo(index);
    } else {
        clearSelection();
        setCurrentIndex(QModelIndex());
    }
    m_quiet = false;
}

QString SchemeGallery::current() const
{
    const QModelIndex index = currentIndex();
    return index.isValid() ? index.data(Qt::DisplayRole).toString() : QString();
}

QSize SchemeGallery::sizeHint() const
{
    // Four cards across, three rows high; it scrolls past that.
    const QFontMetrics fm(font());
    const int cellW = kCardWidth + kGap;
    const int cellH = kCardHeight + fm.height() + 8 + kGap;
    return QSize(4 * cellW + kGap + verticalScrollBar()->sizeHint().width(), 3 * cellH + kGap);
}

void SchemeGallery::changeEvent(QEvent *event)
{
    QListView::changeEvent(event);
    // The System card follows the desktop's colours.
    if (event->type() == QEvent::PaletteChange) {
        const int row = int(m_names.indexOf(ColorSchemeFile::systemName()));
        if (row >= 0)
            m_model->item(row)->setData(QVariant::fromValue(coloursOf(m_names.at(row), palette())), ColoursRole);
    }
}
