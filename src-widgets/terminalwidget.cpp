/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminalwidget.h"

#include "colorscheme.h"
#include "procinfo.h"
#include "screenshotoverlay.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <unistd.h>   // getpgid

#include <Emulation.h>
#include <TerminalDisplay.h>
#include <qtermwidget.h>

namespace {
// Width of the frame round a pane in a split tab.
constexpr int kPaneBorder = 2;
} // namespace

TerminalWidget::TerminalWidget(int id, const Settings &settings,
                               const TerminalProfile &profile, QWidget *parent,
                               Backend backend)
    : QWidget(parent)
    , m_id(id)
    , m_backend(backend)
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

    if (m_backend == Backend::External) {
        // No process: whoever owns this terminal feeds its output and takes
        // its keystrokes. The source (tmux) has already answered any terminal
        // queries in the output, so the emulator must not answer again.
        m_terminal->setAnswerTerminalQueries(false);
        m_terminal->startTerminalTeletype();
        connect(m_terminal, &QTermWidget::sendData, this, [this](const char *data, int len) {
            emit externalInput(QByteArray(data, len));
        });
        return;
    }

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
    if (!m_profile.environment.isEmpty())
        m_terminal->setEnvironment(m_profile.environment);   // added to superterm's own

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

    /*
     * Settings may hold either a bare scheme name or a full path to a user
     * scheme (that is what the editor stores, because a bare name only resolves
     * if the process has already loaded it).
     *
     * Normalise to a path when one exists: QTermWidget::setColorScheme()
     * silently falls back to the DEFAULT scheme for a name it does not know,
     * which would look like "the scheme reverted" with no error. Profiles store
     * bare names, so they need the same resolution.
     */
    QString resolved = scheme;
    QString display = QFileInfo(scheme).baseName();
    if (scheme.contains(QLatin1Char('/'))) {
        if (!QFile::exists(scheme))
            resolved = ColorSchemeFile::findSchemeFile(display);
    } else {
        const QString file = ColorSchemeFile::findSchemeFile(scheme);
        if (!file.isEmpty())
            resolved = file;
    }
    if (resolved.isEmpty())
        resolved = scheme;

    /*
     * A live stand-in (used so an in-place scheme edit bypasses qtermwidget's
     * basename cache) has a throwaway filename. Show the real scheme it stands
     * in for, not "_live_1700000000000_1".
     */
    if (display.startsWith(QLatin1String("_live_")))
        display = m_settings.liveSchemeRealName();
    if (display.contains(QLatin1Char('/')))
        display = QFileInfo(display).baseName();

    m_appliedColorScheme = display;
    m_terminal->setColorScheme(resolved);

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

    // Breathing room around the terminal grid. Not for external panes: their
    // size must map exactly onto the cells their source (tmux) lays out, so
    // the tab pads round the whole layout instead.
    m_terminal->setMargin(isExternal() ? 0 : a.terminalMargin);

    // Bidirectional text rendering (Konsole's "bidi").
    m_terminal->setBidiEnabled(a.bidi);

    // Background opacity. The old QML build surfaced this; the widgets API
    // supports it too, so the shared key finally does something here.
    m_terminal->setTerminalOpacity(a.opacity);

    // Extra pixels between rows. QTermWidget has no setter for this, but its
    // display does. Only set on change: it re-applies the font, which reflows.
    if (auto *d = this->display(); d && d->lineSpacing() != uint(qMax(0, a.lineSpacing)))
        d->setLineSpacing(uint(qMax(0, a.lineSpacing)));
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
    // External panes have none, for the same reason as their margin (see
    // applyAppearance()); scrolling back still works with the wheel and keys.
    m_terminal->setScrollBarPosition(
        isExternal() ? QTermWidgetInterface::NoScrollBar
                     : kPos.value(s.scrollbarPosition, QTermWidgetInterface::ScrollBarRight));

    // When XON/XOFF freezes the display, say so instead of looking hung.
    m_terminal->setFlowControlEnabled(true);
    m_terminal->setFlowControlWarningEnabled(s.flowControlWarning);

    m_terminal->setConfirmMultilinePaste(s.confirmMultilinePaste);
    m_terminal->setTrimPastedTrailingNewlines(s.trimPastedTrailingNewlines);
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

void TerminalWidget::feed(const QByteArray &data)
{
    if (!m_terminal || !isExternal())
        return;
    m_terminal->feedData(data);
}

void TerminalWidget::setScreenSize(int columns, int lines)
{
    if (m_terminal && isExternal())
        m_terminal->setFixedTerminalSize(columns, lines);
}

QSize TerminalWidget::cellSize() const
{
    auto *d = display();
    return d ? QSize(d->fontWidth(), d->fontHeight()) : QSize(8, 16);
}

QSize TerminalWidget::chromeSize() const
{
    const int frame = (m_frame == PaneFrame::None) ? 0 : 2 * kPaneBorder;
    auto *d = display();
    const int margin = d ? 2 * d->margin() : 0;
    // The scrollbar takes width only where the style doesn't overlay it.
    int scrollbar = 0;
    if (d) {
        if (auto *bar = d->findChild<QScrollBar *>(); bar && bar->isVisibleTo(d)
            && !bar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, bar))
            scrollbar = bar->width();
    }
    return {frame + margin + scrollbar, frame + margin};
}

int TerminalWidget::paneBorder()
{
    return kPaneBorder;
}

void TerminalWidget::setPaneFrame(PaneFrame frame)
{
    if (m_frame == frame)
        return;
    m_frame = frame;
    const int m = (frame == PaneFrame::None) ? 0 : kPaneBorder;
    layout()->setContentsMargins(m, m, m, m);
    update();
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    if (m_frame == PaneFrame::None)
        return;
    // The terminal covers everything but the frame, so filling the whole
    // widget only shows in the border.
    QPainter p(this);
    QColor colour = palette().color(QPalette::Highlight);
    if (m_frame == PaneFrame::Inactive)
        colour = display() ? display()->palette().color(QPalette::Window)
                           : palette().color(QPalette::Window);
    p.fillRect(rect(), colour);
}

QString TerminalWidget::currentDirectory() const
{
    if (!m_terminal || isExternal())
        return QString();
    return m_terminal->workingDirectory();
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
    // An external terminal's programs live elsewhere (in tmux), and survive
    // superterm quitting.
    if (!m_terminal || isExternal())
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

    return procinfo::name(m_terminal->getForegroundProcessId());
}

QStringList TerminalWidget::foregroundCommandLine() const
{
    if (!m_terminal || !hasRunningProcess())
        return {};
    return procinfo::arguments(m_terminal->getForegroundProcessId());
}

QSize TerminalWidget::screenPixelSize() const
{
    return m_terminal ? m_terminal->screenPixelSize() : QSize();
}

QPixmap TerminalWidget::previewPixmap(const QSize &size, qreal devicePixelRatio) const
{
    const QSize natural = screenPixelSize();
    if (natural.isEmpty() || size.isEmpty())
        return {};
    // Drawn from the emulator's screen, not grabbed from the display, so a
    // hidden pane previews as well as a visible one. Drawing at twice the
    // final size and scaling down smoothly gives cleaner small text than
    // drawing tiny glyphs directly.
    const qreal scale = qMin(qreal(size.width()) / natural.width(),
                             qreal(size.height()) / natural.height());
    const QImage image = m_terminal->renderScreen(qMin(1.0, 2 * scale * devicePixelRatio));
    if (image.isNull())
        return {};
    QPixmap out = QPixmap::fromImage(image.scaled(
        (QSizeF(natural) * scale * devicePixelRatio).toSize(), Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
    out.setDevicePixelRatio(devicePixelRatio);
    return out;
}

Konsole::TerminalDisplay *TerminalWidget::display() const
{
    return m_terminal ? m_terminal->findChild<Konsole::TerminalDisplay *>() : nullptr;
}

void TerminalWidget::takeScreenshot()
{
    auto *d = display();
    if (!d)
        return;
    // The grid plus the display's own margin on every side, so the text isn't
    // flush against the edge. This excludes the scrollbar (which sits outside
    // the margin) and the sliver of a partial cell at the right and bottom.
    const int m = d->margin();
    const QRect area = d->cellArea().adjusted(-m, -m, m, m).intersected(d->rect());
    emit screenshotTaken(ScreenshotOverlay::crop(d->grab(), area));
}

void TerminalWidget::startRegionScreenshot()
{
    auto *d = display();
    if (!d || d->findChild<ScreenshotOverlay *>())
        return;   // already selecting
    auto *overlay = new ScreenshotOverlay(d, d->cellArea(),
                                          QSize(d->fontWidth(), d->fontHeight()));
    connect(overlay, &ScreenshotOverlay::captured, this, [this](const QPixmap &image) {
        focusTerminal();
        emit screenshotTaken(image);
    });
    connect(overlay, &ScreenshotOverlay::cancelled, this, &TerminalWidget::focusTerminal);
    overlay->show();
    overlay->raise();
    overlay->setFocus();
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
    menu.addAction(tr("Split &Right"), this, [this]() { emit splitRequested(Qt::Horizontal); })
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    menu.addAction(tr("Split &Down"), this, [this]() { emit splitRequested(Qt::Vertical); })
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    menu.addAction(tr("Close Pa&ne"), this, &TerminalWidget::closePaneRequested)
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+X")));

    menu.addSeparator();

    menu.addAction(tr("Screenshot &Region…"), this,
                   &TerminalWidget::startRegionScreenshot)
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    menu.addAction(tr("Screens&hot Terminal"), this, &TerminalWidget::takeScreenshot)
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));

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
