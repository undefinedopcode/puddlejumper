/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "vnctab.h"

#include "vncclient.h"
#include "vncview.h"

#include <QPainter>
#include <QVBoxLayout>

VncTab::VncTab(const QString &host, quint16 port, QWidget *parent)
    : Tab(parent), m_view(new VncView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    setFocusProxy(m_view);

    VncClient *client = m_view->client();
    connect(m_view, &VncView::titleChanged, this, &VncTab::changed);
    connect(client, &VncClient::resized, this, &VncTab::changed);
    connect(client, &VncClient::updated, this, [this]() {
        m_newOutput = true;
        emit screenUpdated();
    });
    m_view->connectToHost(host, port);
}

QString VncTab::address() const
{
    return m_view->address();
}

QString VncTab::title() const
{
    return m_view->title();
}

bool VncTab::isConnected() const
{
    return m_view->client()->isConnected();
}

QString VncTab::details() const
{
    if (!isConnected())
        return tr("%1  ·  not connected").arg(address());
    const QSize size = m_view->client()->framebuffer().size();
    return tr("%1  ·  %2 × %3").arg(address()).arg(size.width()).arg(size.height());
}

QPixmap VncTab::previewPixmap(const QSize &size, qreal devicePixelRatio) const
{
    QPixmap out(size * devicePixelRatio);
    out.setDevicePixelRatio(devicePixelRatio);
    out.fill(backgroundColour());
    const QImage &screen = m_view->client()->framebuffer();
    if (!isConnected() || screen.isNull())
        return out;
    // The whole screen, letterboxed in the card's shape.
    const QSizeF fitted = QSizeF(screen.size()).scaled(QSizeF(size), Qt::KeepAspectRatio);
    const QRectF box(QPointF((size.width() - fitted.width()) / 2,
                             (size.height() - fitted.height()) / 2), fitted);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(box, screen);
    return out;
}

QColor VncTab::backgroundColour() const
{
    return Qt::black;
}

QColor VncTab::foregroundColour() const
{
    return QColor(0xc0, 0xc0, 0xc0);
}

void VncTab::focusContent()
{
    m_view->setFocus();
}
