/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QWidget>

#include "settings.h"

class QTermWidget;

/*
 * One terminal tab.
 *
 * Bundles a QTermWidget with the bits the widgets API does not provide:
 * a stable id (for preview files), the live title, and the ability to re-apply
 * the whole appearance in one call.
 */
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    TerminalWidget(int id, const Settings &settings, QWidget *parent = nullptr);

    int id() const { return m_id; }

    QTermWidget *terminal() const { return m_terminal; }

    QString sessionTitle() const { return m_title; }

    /* Re-apply every appearance setting. Safe to call on live terminals. */
    void applyAppearance();

    /* Re-apply scrollback depth. */
    void applyScrolling();

    void sendText(const QString &text);
    void focusTerminal();

    /* A live preview of the terminal as painted. Empty if unavailable. */
    QPixmap previewPixmap(const QSize &size) const;

signals:
    void titleChanged(const QString &title);
    void sessionFinished();

private:
    int m_id = 0;
    const Settings &m_settings;
    QTermWidget *m_terminal = nullptr;
    QString m_title;
};
