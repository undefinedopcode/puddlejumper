/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminalwidget.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QMenu>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <unistd.h>   // getpgid

#include <Emulation.h>
#include <qtermwidget.h>

TerminalWidget::TerminalWidget(int id, const Settings &settings,
                               const TerminalProfile &profile, QWidget *parent)
    : QWidget(parent)
    , m_id(id)
    , m_settings(settings)
    , m_profile(profile)
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

    // Track whether there is a selection, so the context menu can enable Copy.
    connect(m_terminal, &QTermWidget::copyAvailable, this, [this](bool available) {
        m_canCopy = available;
    });

    // Right-click menu: our own, built from QTermWidget's public actions. It
    // is assembled on demand (see showContextMenu) so Copy reflects selection.
    m_terminal->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_terminal, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                showContextMenu(m_terminal->mapToGlobal(pos));
            });

    // receivedData fires from the pty, so it also fires while this tab is
    // hidden. That is what lets the sidebar know a background tab has changed
    // without having to look at it.
    connect(m_terminal, &QTermWidget::receivedData, this, [this]() {
        ++m_outputSeq;
        m_dirty = true;
        onOutput();
    });

    // Monitor signals: pass them on only when the matching setting is on, so
    // the owner never has to care whether monitoring is enabled.
    connect(m_terminal, &QTermWidget::activity, this, [this]() {
        if (m_settings.scrolling().monitorActivity)
            emit activityDetected();
    });
    connect(m_terminal, &QTermWidget::silence, this, [this]() {
        if (m_settings.scrolling().monitorSilence)
            emit silenceDetected();
    });
    applyMonitoring();
    connect(&m_settings, &Settings::monitoringChanged,
            this, &TerminalWidget::applyMonitoring);

    // Optional shell override, useful for testing:
    //   SUPERTERM_SHELL=/bin/fish superterm
    //
    // The environment variable wins over the profile: it is an explicit,
    // per-run instruction, and tools/tests rely on it to pin a shell without
    // editing config.
    const QByteArray shell = qgetenv("SUPERTERM_SHELL");
    const QString envShell = QString::fromLocal8Bit(shell);

    const QString program =
        !envShell.isEmpty() ? envShell
                            : (m_profile.shellProgram ? *m_profile.shellProgram
                                                      : QString());
    if (!program.isEmpty())
        m_terminal->setShellProgram(program);

    // Arguments only make sense with an explicit program; passing them to the
    // login shell would silently change its behaviour.
    if (envShell.isEmpty() && m_profile.shellProgram && m_profile.shellArgs)
        m_terminal->setArgs(*m_profile.shellArgs);

    if (m_profile.workingDirectory && !m_profile.workingDirectory->isEmpty())
        m_terminal->setWorkingDirectory(*m_profile.workingDirectory);

    m_terminal->startShellProgram();
}

void TerminalWidget::setProfile(const TerminalProfile &profile)
{
    m_profile = profile;
    // The running shell is left alone -- relaunching it would destroy the
    // session -- but the appearance is ours to update.
    applyAppearance();
}

double TerminalWidget::appliedFontSize() const
{
    return m_profile.fontSize ? *m_profile.fontSize
                              : m_settings.appearance().fontSize;
}

void TerminalWidget::onOutput()
{
    // receivedData can fire many times a second; report at most a few times a
    // second so the window is not woken up for every chunk of output.
    static constexpr quint64 kReportEvery = 4;
    if (m_outputSeq - m_reportedSeq < kReportEvery)
        return;
    m_reportedSeq = m_outputSeq;
    emit outputArrived();
}

void TerminalWidget::applyAppearance()
{
    if (!m_terminal)
        return;

    const auto &a = m_settings.appearance();

    /*
     * Resolve the profile against the globals.
     *
     * This runs on every appearance change, so each field is decided
     * independently: an overridden field keeps its profile value, an inherited
     * one picks up the new global. That is what makes "themed Dev tab" and
     * "global font size" coexist without fighting.
     */
    const QString scheme = m_profile.colorScheme ? *m_profile.colorScheme
                                                 : a.colorScheme;
    m_appliedColorScheme = scheme;
    m_terminal->setColorScheme(scheme);

    QFont font(m_profile.fontFamily ? *m_profile.fontFamily : a.fontFamily);
    // setPointSizeF is honoured: measured 13 / 13.5 / 14pt producing distinct
    // cell grids (79x19 / 72x19 / 72x18), so half-point steps are real.
    font.setPointSizeF(m_profile.fontSize ? *m_profile.fontSize : a.fontSize);
    m_terminal->setTerminalFont(font);

    m_terminal->setBoldIntense(a.boldIntense);
    m_terminal->setBlinkingCursor(a.blinkingCursor);
    // Draw box/block characters as vectors. The library defaults this to true,
    // so setting it is only meaningful when the user has turned it off.
    m_terminal->setDrawLineChars(a.drawLineChars);

    // Cursor glyph. Settings stores 0/1/2; Konsole's KeyboardCursorShape is
    // BlockCursor/UnderlineCursor/IBeamCursor in that order.
    static const Konsole::Emulation::KeyboardCursorShape kShapes[] = {
        Konsole::Emulation::KeyboardCursorShape::BlockCursor,
        Konsole::Emulation::KeyboardCursorShape::UnderlineCursor,
        Konsole::Emulation::KeyboardCursorShape::IBeamCursor,
    };
    m_terminal->setKeyboardCursorShape(kShapes[qBound(0, a.cursorShape, 2)]);

    // Breathing room around the terminal grid.
    m_terminal->setMargin(a.terminalMargin);

    // Bidirectional text rendering (Konsole's "bidi").
    m_terminal->setBidiEnabled(a.bidi);

    // Background opacity. The old QML build surfaced this; the widgets API
    // supports it too, so the shared key finally does something here.
    m_terminal->setTerminalOpacity(a.opacity);

    Q_UNUSED(a.lineSpacing) // QTermWidget has no line-spacing setter.
}

void TerminalWidget::applyScrolling()
{
    if (!m_terminal)
        return;

    const auto &s = m_settings.scrolling();
    m_terminal->setHistorySize(s.scrollback);

    // Scrollbar placement. "none" hides it (useful with the thumbnail sidebar,
    // where the card's preview already shows position).
    static const QHash<QString, QTermWidgetInterface::ScrollBarPosition> kPos = {
        { QStringLiteral("none"),  QTermWidgetInterface::NoScrollBar   },
        { QStringLiteral("left"),  QTermWidgetInterface::ScrollBarLeft  },
        { QStringLiteral("right"), QTermWidgetInterface::ScrollBarRight },
    };
    m_terminal->setScrollBarPosition(
        kPos.value(s.scrollbarPosition, QTermWidgetInterface::ScrollBarRight));

    // When XON/XOFF freezes the display, say so instead of looking hung.
    m_terminal->setFlowControlEnabled(true);
    m_terminal->setFlowControlWarningEnabled(s.flowControlWarning);
}

void TerminalWidget::applyMonitoring()
{
    if (!m_terminal)
        return;

    const auto &s = m_settings.scrolling();

    m_terminal->setMonitorActivity(s.monitorActivity);
    m_terminal->setMonitorSilence(s.monitorSilence);
    m_terminal->setSilenceTimeout(s.silenceTimeout);
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

/*
 * Is something running in this terminal that quitting would kill?
 *
 * The pty's foreground process group is read with tcgetpgrp(). When it equals
 * the shell's own group, the shell is the one reading the terminal -- an idle
 * prompt, or one with only a background job. When it differs, a foreground
 * program owns the tty and would be killed by a quit.
 *
 * Both ids can legitimately be 0 (no pty / session not started yet); that
 * reports false rather than warning about a terminal that does not exist.
 */
bool TerminalWidget::hasRunningProcess() const
{
    if (!m_terminal)
        return false;

    const int shellPid = m_terminal->getShellPID();
    const int fgPgrp = m_terminal->getForegroundProcessId();
    if (shellPid <= 0 || fgPgrp <= 0)
        return false;

    // getpgid() rather than a direct shellPid comparison, so a shell that is
    // not its own group leader is still recognised as "at the prompt".
    const int shellPgrp = ::getpgid(shellPid);
    const int reference = (shellPgrp > 0) ? shellPgrp : shellPid;
    return fgPgrp != reference;
}

QString TerminalWidget::foregroundProgramName() const
{
    if (!m_terminal)
        return QString();

    const int fgPgrp = m_terminal->getForegroundProcessId();
    if (fgPgrp <= 0)
        return QString();

    // /proc/<pid>/comm is the kernel's short name for the process, truncated to
    // 15 characters. Read it directly rather than shelling out to ps.
    QFile comm(QStringLiteral("/proc/%1/comm").arg(fgPgrp));
    if (!comm.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(comm.readAll()).trimmed();
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

void TerminalWidget::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);

    // Copy is enabled only while the terminal reports a live selection.
    QAction *copyAct = menu.addAction(tr("&Copy"), m_terminal, &QTermWidget::copyClipboard);
    copyAct->setEnabled(m_canCopy);
    copyAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));

    QAction *pasteAct = menu.addAction(tr("&Paste"), m_terminal, &QTermWidget::pasteClipboard);
    pasteAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));

    menu.addSeparator();

    QAction *findAct = menu.addAction(tr("&Find…"), m_terminal,
                                      &QTermWidget::toggleShowSearchBar);
    findAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));

    QAction *clearAct = menu.addAction(tr("C&lear"), m_terminal, &QTermWidget::clear);

    menu.addSeparator();

    // Write the full scrollback (not just the visible screen) to a file.
    // Writes in plain text — exactly what Konsole's "Save Output As" does.
    menu.addAction(tr("&Save Output As…"), this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Terminal Output"), QString(),
            tr("Text files (*.txt);;All files (*)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return;
        m_terminal->saveHistory(&file);
    });

    menu.addSeparator();

    // Zoom acts on the shared Settings via the owner, which propagates to
    // every open tab (and persists).
    QAction *zoomInAct = menu.addAction(tr("Zoom &In"), this,
                                        &TerminalWidget::zoomInRequested);
    zoomInAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+=")));

    QAction *zoomOutAct = menu.addAction(tr("Zoom &Out"), this,
                                         &TerminalWidget::zoomOutRequested);
    zoomOutAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));

    QAction *zoomResetAct = menu.addAction(tr("&Reset Zoom"), this,
                                           &TerminalWidget::zoomResetRequested);
    zoomResetAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));

    menu.exec(globalPos);
}
