/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

/*
 * Laid over a Rex window's tab while its server isn't connected: what
 * happened and what is being done about it, with Reconnect and Close. It
 * covers the panes (the last screen they showed stays under a scrim) and
 * keeps itself the size of the tab.
 */
class RexOfflineOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit RexOfflineOverlay(QWidget *tab);

    /* The heading, the line under it, and the reconnect button's text. */
    void setState(const QString &title, const QString &detail, const QString &reconnectText);

signals:
    void reconnectRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void fit();

    QWidget *m_card = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_detail = nullptr;
    QPushButton *m_reconnect = nullptr;
};
