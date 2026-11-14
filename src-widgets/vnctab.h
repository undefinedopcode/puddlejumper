/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include "tab.h"

class VncView;

/*
 * A tab showing a remote screen over VNC (see VncView). Its card shows the
 * screen; new output is any change to it.
 */
class VncTab : public Tab
{
    Q_OBJECT

public:
    explicit VncTab(const QString &host, quint16 port, QWidget *parent = nullptr);

    VncView *view() const { return m_view; }
    /* host:port, as connected to (and saved). */
    QString address() const;
    /* The remote desktop's name, or the address until it has said. */
    QString title() const;
    bool isConnected() const;
    /* Size and state, for the status bar and the card. */
    QString details() const;

    bool hasNewOutput() const override { return m_newOutput; }
    void clearNewOutput() override { m_newOutput = false; }
    QPixmap previewPixmap(const QSize &size, qreal devicePixelRatio = 1.0) const override;
    QColor backgroundColour() const override;
    QColor foregroundColour() const override;
    void focusContent() override;

signals:
    /* The title, connection or screen size changed. */
    void changed();
    /* The screen changed. */
    void screenUpdated();

private:
    VncView *m_view = nullptr;
    bool m_newOutput = true;
};
