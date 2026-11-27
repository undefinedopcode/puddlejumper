/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "rexoffline.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

RexOfflineOverlay::RexOfflineOverlay(QWidget *tab)
    : QWidget(tab)
{
    setObjectName(QStringLiteral("rexOfflineOverlay"));
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::StrongFocus);   // keys stop here, not in the panes under it

    auto *card = new QFrame(this);
    m_card = card;
    card->setObjectName(QStringLiteral("rexOfflineCard"));
    card->setFrameShape(QFrame::StyledPanel);
    card->setAutoFillBackground(true);

    m_title = new QLabel(card);
    QFont f = m_title->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.15);
    m_title->setFont(f);
    m_title->setWordWrap(true);
    m_title->setAlignment(Qt::AlignCenter);
    m_detail = new QLabel(card);
    m_detail->setWordWrap(true);
    m_detail->setAlignment(Qt::AlignCenter);
    m_detail->setTextFormat(Qt::PlainText);

    m_reconnect = new QPushButton(card);
    m_reconnect->setDefault(true);
    auto *close = new QPushButton(tr("Close Tabs"), card);
    close->setToolTip(tr("Stop showing this session. It stays on the server; attach to it again from the Rex menu."));
    connect(m_reconnect, &QPushButton::clicked, this, &RexOfflineOverlay::reconnectRequested);
    connect(close, &QPushButton::clicked, this, &RexOfflineOverlay::closeRequested);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_reconnect);
    buttons->addWidget(close);
    buttons->addStretch();

    auto *inner = new QVBoxLayout(card);
    inner->setContentsMargins(24, 20, 24, 20);
    inner->setSpacing(10);
    inner->addWidget(m_title);
    inner->addWidget(m_detail);
    inner->addSpacing(4);
    inner->addLayout(buttons);

    auto *outer = new QVBoxLayout(this);
    outer->addStretch();
    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch();

    tab->installEventFilter(this);
    fit();
    raise();
    show();
}

void RexOfflineOverlay::fit()
{
    // A definite width, so the wrapped lines get the height they need.
    setGeometry(parentWidget()->rect());
    m_card->setFixedWidth(qBound(200, width() - 32, 480));
}

void RexOfflineOverlay::setState(const QString &title, const QString &detail, const QString &reconnectText)
{
    m_title->setText(title);
    m_detail->setText(detail);
    m_reconnect->setText(reconnectText);
}

bool RexOfflineOverlay::eventFilter(QObject *watched, QEvent *event)
{
    // The tab's size, and above whatever the tab adds later (a relayout
    // rebuilds its splitters and floating frames).
    if (watched == parent() && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded
                                || event->type() == QEvent::LayoutRequest)) {
        fit();
        raise();
    }
    return QWidget::eventFilter(watched, event);
}

void RexOfflineOverlay::paintEvent(QPaintEvent *)
{
    // A scrim over the last screen the panes showed.
    QPainter p(this);
    QColor scrim = palette().color(QPalette::Window);
    scrim.setAlpha(205);
    p.fillRect(rect(), scrim);
}
