/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminalwidget.h"

#include <QFont>
#include <QVBoxLayout>
#include <QWidget>

#include <Emulation.h>
#include <qtermwidget.h>

TerminalWidget::TerminalWidget(int id, const Settings &settings, QWidget *parent)
    : QWidget(parent)
    , m_id(id)
    , m_settings(settings)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_terminal = new QTermWidget(0, this);
    layout->addWidget(m_terminal);

    applyAppearance();
    applyScrolling();
    m_terminal->setKeyBindings(QStringLiteral("default"));

    connect(m_terminal, &QTermWidget::titleChanged, this, [this]() {
        const QString t = m_terminal->title().trimmed();
        // A program can clear its title; keep the last useful one.
        if (t.isEmpty() || t == m_title)
            return;
        m_title = t;
        emit titleChanged(m_title);
    });

    connect(m_terminal, &QTermWidget::finished, this, [this]() {
        emit sessionFinished();
    });

    m_terminal->startShellProgram();
}

void TerminalWidget::applyAppearance()
{
    if (!m_terminal)
        return;

    const auto &a = m_settings.appearance();

    m_terminal->setColorScheme(a.colorScheme);

    QFont font(a.fontFamily);
    // setPointSizeF is honoured: measured 13 / 13.5 / 14pt producing distinct
    // cell grids (79x19 / 72x19 / 72x18), so half-point steps are real.
    font.setPointSizeF(a.fontSize);
    m_terminal->setTerminalFont(font);

    m_terminal->setBoldIntense(a.boldIntense);
    m_terminal->setBlinkingCursor(a.blinkingCursor);
    // Not exposed as a property; these are the calls the QML build could not
    // make at all.
    m_terminal->setDrawLineChars(true);
    Q_UNUSED(a.lineSpacing) // QTermWidget has no line-spacing setter.
}

void TerminalWidget::applyScrolling()
{
    if (!m_terminal)
        return;
    m_terminal->setHistorySize(m_settings.scrolling().scrollback);
}

void TerminalWidget::sendText(const QString &text)
{
    if (m_terminal)
        m_terminal->sendText(text);
}

void TerminalWidget::focusTerminal()
{
    if (m_terminal)
        m_terminal->setFocus();
}

QPixmap TerminalWidget::previewPixmap(const QSize &size) const
{
    if (!m_terminal)
        return {};
    // Widgets paint to a backing store, so grabbing is safe here -- unlike the
    // QML build, which needed useFBORendering:false to avoid blanking the
    // terminal on every grab.
    const auto widgets = m_terminal->findChildren<QWidget *>();
    for (QWidget *w : widgets) {
        if (QString::fromLatin1(w->metaObject()->className())
                .contains(QLatin1String("TerminalDisplay"))) {
            return w->grab().scaled(size, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        }
    }
    return {};
}
