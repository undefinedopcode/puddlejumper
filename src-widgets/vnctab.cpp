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

VncTab::VncTab(const QString &host, quint16 port, bool reducedColour, QWidget *parent,
               RemoteClient *client)
    : Tab(parent), m_view(new VncView(this, client))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
    setFocusProxy(m_view);

    client = m_view->client();
    connect(m_view, &VncView::titleChanged, this, &VncTab::changed);
    connect(client, &RemoteClient::resized, this, &VncTab::changed);
    connect(client, &RemoteClient::updated, this, [this]() {
        m_newOutput = true;
        emit screenUpdated();
    });
    m_view->setReducedColour(reducedColour);
    m_view->connectToHost(host, port);
}

bool VncTab::reducedColour() const
{
    return m_view->client()->reducedColour();
}

void VncTab::setReducedColour(bool on)
{
    m_view->setReducedColour(on);
    emit changed();
}

bool VncTab::adaptiveQuality() const
{
    return m_view->client()->adaptiveQuality();
}

void VncTab::setAdaptiveQuality(bool on)
{
    m_view->setAdaptiveQuality(on);
    emit changed();
}

QString VncTab::address() const
{
    return m_view->address();
}

QString VncTab::username() const
{
    return m_view->username();
}

void VncTab::setUsername(const QString &username)
{
    m_view->setUsername(username);
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
    QString text = tr("%1  ·  %2 × %3").arg(address()).arg(size.width()).arg(size.height());
    if (reducedColour())
        text += tr("  ·  16-bit colour");
    return text;
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
