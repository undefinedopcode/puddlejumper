/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "terminalwidget.h"

#include "cardstyle.h"
#include "colorscheme.h"
#include "formername.h"
#include "procinfo.h"
#include "programresume.h"
#include "programstatus.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QDropEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QDir>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifndef Q_OS_WIN
#include <unistd.h>   // getpgid
#endif

#include <Emulation.h>
#include <TerminalDisplay.h>
#include <qtermwidget.h>

namespace {
// Width of the frame round a pane in a split tab.
constexpr int kPaneBorder = 2;
// Pane cards: the panel's edge on each side, and its title strip.
constexpr int kCardSide = 3;
constexpr int kCardTitle = 22;  // as a float's title strip (kFloatTitle)
constexpr int kCardRadius = 6;
bool s_paneCards = false;
int s_margin = 0;   // the terminal margin (appearance), for externalChrome()
bool s_quietFocus = false;
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
    if (auto *d = display())
        d->installEventFilter(this);  // pointerPressed()

    connect(m_terminal, &QTermWidget::titleChanged, this, [this]() {
        const QString t = m_terminal->title().trimmed();
        // A program can clear its title; keep the last useful one.
        if (t.isEmpty() || t == m_title)
            return;
        m_title = t;
        if (isCard()) {
            refreshCardContext();  // a shell retitles at each prompt: after a cd, a program's end
            update();              // the card's title strip
        }
        emit titleChanged(m_title);
    });

    connect(m_terminal, &QTermWidget::finished, this, [this]() {
        emit sessionFinished();
    });

    m_status = new ProgramStatus(this);
    m_statusWatch = new QTimer(this);
    m_statusWatch->setInterval(500);
    connect(m_statusWatch, &QTimer::timeout, this, &TerminalWidget::checkProgramStatusOwner);
    connect(m_terminal, &QTermWidget::programStatusReport, this, &TerminalWidget::onProgramStatusReport);
    connect(m_terminal, &QTermWidget::clipboardWritten, this, &TerminalWidget::clipboardWritten);
    connect(m_terminal, &QTermWidget::fullReset, m_status, &ProgramStatus::reset);
    connect(m_terminal, &QTermWidget::progressReport, this, [this](int state, int progress) {
        if (m_status->applyProgress(state, progress))
            watchProgramStatusOwner();
    });
    // A shell with semantic prompts (OSC 133) says when it is back.
    connect(m_terminal, &QTermWidget::promptStarted, this, [this]() {
        m_promptMarks = true;
        programEnded();
    });
    connect(m_terminal, &QTermWidget::programResumeReport, this, [this](const QString &body) {
        if (m_resume)
            m_resume->apply(body, m_terminal->getShellPID());
    });
    // Back at this terminal: what finished here has been seen.
    connect(m_terminal, &QTermWidget::termKeyPressed, m_status, &ProgramStatus::markSeen);

    // A link's hot spot fires at any click on it, which mostly means to
    // select or to place the cursor: only a Ctrl-click (Cmd on a Mac) opens it.
    connect(m_terminal, &QTermWidget::urlActivated, this, [this](const QUrl &url, bool fromMenu) {
        if (fromMenu || QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
            emit linkOpened(url);
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
        // Its size is the source's layout, changing whenever any pane or
        // the window does: no "Size: cols x rows" flash over it each time.
        m_terminal->setTerminalSizeHint(false);
        m_terminal->startTerminalTeletype();
        connect(m_terminal, &QTermWidget::sendData, this, [this](const char *data, int len) {
            emit externalInput(QByteArray(data, len));
        });
        return;
    }

    // Optional shell override, useful for testing:
    //   PUDDLEJUMPER_SHELL=/bin/fish puddlejumper
    //
    // The environment variable wins over the profile: it is an explicit,
    // per-run instruction, and tools/tests rely on it to pin a shell without
    // editing config.
    const QByteArray shell = appEnv("SHELL");
    const QString envShell = QString::fromLocal8Bit(shell);

    const QString program =
        !envShell.isEmpty() ? envShell
                            : (m_profile.shellProgram ? *m_profile.shellProgram
                                                      : QString());
    if (!program.isEmpty())
        m_terminal->setShellProgram(program);
#ifdef Q_OS_MACOS
    // As Terminal does, a login shell: an app opened from the Finder has only
    // launchd's bare PATH, and it's /etc/zprofile (path_helper) and
    // ~/.zprofile (where Homebrew is set up) that add to it.
    if (program.isEmpty()) {
        m_terminal->setShellProgram(qEnvironmentVariable("SHELL", QStringLiteral("/bin/zsh")));
        m_terminal->setArgs({QStringLiteral("-l")});
    }
#endif

    // Arguments only make sense with an explicit program; passing them to the
    // login shell would silently change its behaviour.
    if (envShell.isEmpty() && m_profile.shellProgram && m_profile.shellArgs)
        m_terminal->setArgs(*m_profile.shellArgs);

    if (m_profile.workingDirectory && !m_profile.workingDirectory->isEmpty())
        m_terminal->setWorkingDirectory(*m_profile.workingDirectory);
    // Added to puddlejumper's own: the profile's, and a token for programs
    // to sign how to resume them with (OSC 7577).
    m_resume = std::make_unique<ProgramResume>();
    m_terminal->setEnvironment(m_profile.environment
                               + QStringList{QStringLiteral("OSC7577_TOKEN=") + m_resume->token()});

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
    s_paneCards = m_settings.appearance().paneCards;
    s_quietFocus = m_settings.appearance().quietFocus;
    s_margin = qMax(0, m_settings.appearance().terminalMargin);
    applyFrameMargins();
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

    // Breathing room around the terminal grid, inside the pane, whatever
    // runs it. A tmux or Rex pane is sized to its cells plus this and its
    // frame (externalChrome()), so the server's cells still fit exactly.
    m_terminal->setMargin(a.terminalMargin);

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
    if (!m_terminal || !isExternal())
        return;
    m_terminal->setFixedTerminalSize(columns, lines);
    emit resized();   // the pane itself may keep its size
}

QSize TerminalWidget::cellSize() const
{
    auto *d = display();
    return d ? QSize(d->fontWidth(), d->fontHeight()) : QSize(8, 16);
}

QSize TerminalWidget::chromeSize() const
{
    const QSize frame = (m_frame == PaneFrame::None) ? QSize() : frameChrome(!m_floating);
    auto *d = display();
    const int margin = d ? 2 * d->margin() : 0;
    // The scrollbar takes width only where the style doesn't overlay it.
    int scrollbar = 0;
    if (d) {
        if (auto *bar = d->findChild<QScrollBar *>(); bar && bar->isVisibleTo(d)
            && !bar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, bar))
            scrollbar = bar->width();
    }
    return {frame.width() + margin + scrollbar, frame.height() + margin};
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
    applyFrameMargins();
}

void TerminalWidget::setQuietFocus(bool on)
{
    s_quietFocus = on;
}

bool TerminalWidget::quietFocus()
{
    return s_quietFocus;
}

QColor TerminalWidget::themeBackground() const
{
    if (auto *d = display())
        return d->colorTable()[1].color;
    return palette().color(QPalette::Window);
}

QColor TerminalWidget::frameEdgeColour(const QColor &bg, const QColor &fg, bool active, bool floating)
{
    // Unfocused: a faint edge (a float's plainer, to stand off the panes
    // under it). Focused: the accent, or with a quiet focus the same edge
    // brighter.
    const qreal plain = floating ? 0.62 : 0.2;
    if (!active)
        return cardstyle::mix(bg, fg, plain);
    if (s_quietFocus)
        return cardstyle::mix(bg, fg, floating ? 0.9 : 0.5);
    return floating ? cardstyle::accent() : cardstyle::mix(bg, cardstyle::accent(), 0.7);
}

QColor TerminalWidget::cardStripColour() const
{
    QColor bg = palette().color(QPalette::Window), fg = palette().color(QPalette::WindowText);
    if (auto *d = display()) {
        fg = d->colorTable()[0].color;
        bg = d->colorTable()[1].color;
    }
    const bool active = m_frame == PaneFrame::Active;
    if (active && s_quietFocus)
        return cardstyle::mix(bg, fg, 0.13);
    return cardstyle::mix(bg, active ? cardstyle::accent() : fg, active ? 0.16 : 0.06);
}

void TerminalWidget::setPaneCards(bool on)
{
    s_paneCards = on;
}

bool TerminalWidget::paneCards()
{
    return s_paneCards;
}

void TerminalWidget::setFallbackTitle(const QString &title)
{
    if (m_fallbackTitle == title)
        return;
    m_fallbackTitle = title;
    if (isCard())
        update();
}

void TerminalWidget::setFloating(bool floating)
{
    if (m_floating == floating)
        return;
    m_floating = floating;
    applyFrameMargins();
}

QSize TerminalWidget::frameChrome(bool tiled)
{
    if (s_paneCards && tiled)
        return {2 * kCardSide, kCardTitle + kCardSide};
    return {2 * kPaneBorder, 2 * kPaneBorder};
}

QSize TerminalWidget::externalChrome(bool tiled)
{
    return frameChrome(tiled) + QSize(2 * s_margin, 2 * s_margin);
}

int TerminalWidget::cardTitleHeight()
{
    return kCardTitle;
}

bool TerminalWidget::isCard() const
{
    return s_paneCards && !m_floating && m_frame != PaneFrame::None;
}

void TerminalWidget::applyFrameMargins()
{
    if (m_frame == PaneFrame::None)
        layout()->setContentsMargins(0, 0, 0, 0);
    else if (isCard())
        layout()->setContentsMargins(kCardSide, kCardTitle, kCardSide, kCardSide);
    else
        layout()->setContentsMargins(kPaneBorder, kPaneBorder, kPaneBorder, kPaneBorder);
    // A local card names what's running and where, kept up to date while
    // it's a card (a tmux or Rex pane's server names it instead).
    const bool watch = isCard() && m_terminal && !isExternal();
    if (watch && !m_cardWatch) {
        m_cardWatch = new QTimer(this);
        m_cardWatch->setInterval(1500);
        connect(m_cardWatch, &QTimer::timeout, this, &TerminalWidget::refreshCardContext);
    }
    if (m_cardWatch) {
        if (watch) {
            m_cardWatch->start();
            refreshCardContext();
        } else {
            m_cardWatch->stop();
        }
    }
    placeFollowBadge();
    update();
}

QString TerminalWidget::cardTitle() const
{
    if (!m_cardProgram.isEmpty() || !m_cardDir.isEmpty()) {
        QStringList parts;
        for (const QString &part : {m_cardProgram, m_cardDir})
            if (!part.isEmpty())
                parts << part;
        return parts.join(QStringLiteral("  ·  "));
    }
    return !m_title.isEmpty() ? m_title : !m_fallbackTitle.isEmpty() ? m_fallbackTitle : tr("Terminal");
}

void TerminalWidget::refreshCardContext()
{
    if (!isCard() || !m_terminal || isExternal())
        return;
    const QString program = foregroundProgramName();
    QString dir = currentDirectory();
    const QString home = QDir::homePath();
    if (dir == home)
        dir = QStringLiteral("~");
    else if (dir.startsWith(home + QLatin1Char('/')))
        dir = QLatin1Char('~') + dir.mid(home.size());
    if (program == m_cardProgram && dir == m_cardDir)
        return;
    m_cardProgram = program;
    m_cardDir = dir;
    update();
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeFollowBadge();
    emit resized();
}

bool TerminalWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && watched == display())
        emit pointerPressed();
    if (event->type() == QEvent::Drop && watched == display() && m_fileDrop) {
        auto *drop = static_cast<QDropEvent *>(event);
        QStringList paths;
        for (const QUrl &url : drop->mimeData()->urls())
            if (url.isLocalFile())
                paths.append(url.toLocalFile());
        if (!paths.isEmpty() && m_fileDrop(paths)) {
            drop->acceptProposedAction();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalWidget::setFollowing(const QString &note)
{
    if (note.isEmpty()) {
        if (m_followBadge)
            m_followBadge->hide();
        return;
    }
    if (!m_followBadge) {
        m_followBadge = new QLabel(this);
        m_followBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_followBadge->setObjectName(QStringLiteral("followBadge"));
    }
    // In the terminal's own colours, a little lifted off its background,
    // with an outline that reads on light and dark schemes alike.
    QColor fg = palette().color(QPalette::WindowText), bg = palette().color(QPalette::Window);
    if (auto *d = display()) {
        fg = d->colorTable()[0].color;
        bg = d->colorTable()[1].color;
    }
    // The accent outlines it, as it does the active pane and floats.
    const QColor accent = cardstyle::accent();
    m_followBadge->setStyleSheet(QStringLiteral("QLabel#followBadge { color: %1; background: %2; border: 1.5px solid %3;"
                                                " border-radius: 10px; padding: 3px 10px; }")
                                     .arg(fg.name(), cardstyle::mix(bg, accent, 0.28).name(), accent.name()));
    QFont f = font();
    f.setPointSizeF(qMax(8.0, f.pointSizeF() * 0.92));
    f.setWeight(QFont::DemiBold);
    m_followBadge->setFont(f);
    m_followBadge->setText(note);
    m_followBadge->adjustSize();
    placeFollowBadge();
    m_followBadge->show();
    m_followBadge->raise();
}

bool TerminalWidget::isFollowing() const
{
    return m_followBadge && !m_followBadge->isHidden();
}

void TerminalWidget::placeFollowBadge()
{
    if (!m_followBadge)
        return;
    const int inset = 6 + (m_frame == PaneFrame::None ? 0 : isCard() ? kCardTitle : kPaneBorder);
    m_followBadge->adjustSize();
    m_followBadge->move(qMax(inset, width() - m_followBadge->width() - inset - 10), inset);
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    if (m_frame == PaneFrame::None)
        return;
    if (isCard()) {
        // As a floating pane's frame, quieter: the terminal's background,
        // a rounded panel with a faint edge and title strip, the accent
        // (softened) when active.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor bg = palette().color(QPalette::Window), fg = palette().color(QPalette::WindowText);
        if (auto *d = display()) {
            fg = d->colorTable()[0].color;
            bg = d->colorTable()[1].color;
        }
        const bool active = m_frame == PaneFrame::Active;
        const QColor edge = frameEdgeColour(bg, fg, active, false);
        // Round corners onto the surface the cards sit on (the sidebar's,
        // from the scheme's own background, not a focus tint).
        p.fillRect(rect(), cardstyle::surface(themeBackground(), fg));
        const QRectF panel = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
        QPainterPath path;
        path.addRoundedRect(panel, kCardRadius, kCardRadius);
        p.fillPath(path, bg);
        p.save();
        p.setClipPath(path);
        p.fillRect(QRectF(panel.left(), panel.top(), panel.width(), kCardTitle), cardStripColour());
        p.restore();
        p.setPen(QPen(edge, active ? 1.5 : 1.0));
        p.drawPath(path);
        const QString title = cardTitle();
        QFont f = font();
        f.setPointSizeF(qMax(7.0, f.pointSizeF() * 0.85));  // as a float's title
        p.setFont(f);
        const QFontMetrics fm(f);
        QRect strip(kCardRadius + 2, 0, width() - 2 * (kCardRadius + 2), kCardTitle);
        const QColor strong = cardstyle::mix(bg, fg, active ? 0.85 : 0.6);
        const QColor dim = cardstyle::mix(bg, fg, active ? 0.55 : 0.38);
        if (!m_cardProgram.isEmpty() || !m_cardDir.isEmpty()) {
            // Local: the program, then (dimmer) where it is.
            QString program = m_cardProgram;
            if (!program.isEmpty()) {
                program = fm.elidedText(program, Qt::ElideRight, strip.width());
                p.setPen(strong);
                p.drawText(strip, Qt::AlignVCenter | Qt::AlignLeft, program);
                strip.setLeft(strip.left() + fm.horizontalAdvance(program));
            }
            if (!m_cardDir.isEmpty() && strip.width() > fm.horizontalAdvance(QStringLiteral(" · …"))) {
                const QString dir = (program.isEmpty() ? QString() : QStringLiteral("  ·  ")) + m_cardDir;
                p.setPen(dim);
                p.drawText(strip, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(dir, Qt::ElideMiddle, strip.width()));
            }
        } else {
            p.setPen(strong);
            p.drawText(strip, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(title, Qt::ElideRight, strip.width()));
        }
        return;
    }
    // The terminal covers everything but the frame, so filling the whole
    // widget only shows in the border.
    QPainter p(this);
    QColor colour = palette().color(QPalette::Highlight);
    if (m_frame == PaneFrame::Inactive)
        colour = themeBackground();
    else if (s_quietFocus)  // a brighter edge than the accent's
        colour = cardstyle::mix(themeBackground(), display() ? display()->colorTable()[0].color
                                                             : palette().color(QPalette::WindowText), 0.45);
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
    // puddlejumper quitting.
    if (!m_terminal || isExternal())
        return false;

    const int shellPid = m_terminal->getShellPID();
    const int fgPgrp = m_terminal->getForegroundProcessId();
    if (shellPid <= 0 || fgPgrp <= 0)
        return false;

#ifdef Q_OS_WIN
    // No process groups there: the "foreground" is the newest process under
    // the shell, or the shell itself at its prompt.
    return fgPgrp != shellPid;
#else
    // getpgid() rather than a direct shellPid comparison, so a shell that is
    // not its own group leader is still recognised as "at the prompt".
    const int shellPgrp = ::getpgid(shellPid);
    const int reference = (shellPgrp > 0) ? shellPgrp : shellPid;
    return fgPgrp != reference;
#endif
}

void TerminalWidget::onProgramStatusReport(const QString &body)
{
    if (m_status->apply(body))
        watchProgramStatusOwner();
}

void TerminalWidget::setSourceHandlesClipboard(bool handles)
{
    if (m_terminal)
        m_terminal->setClipboardWrites(!handles);
}

void TerminalWidget::setSourceAnswersProgramStatus(bool answers)
{
    if (isExternal())
        m_terminal->setAnswerProgramStatusQuery(!answers);
}

resumerules::Verdict TerminalWidget::resumeVerdict(QString *cwd)
{
    if (!m_resume || !m_terminal)
        return {};
    const int shell = m_terminal->getShellPID();
    const auto records = m_resume->records(shell);
    if (records.isEmpty())
        return {};
    const ProgramResume::Record &r = records.first();
    const QString dir = !r.cwd.isEmpty() ? r.cwd : procinfo::workingDirectory(r.pid);
    if (cwd)
        *cwd = dir;
    return resumerules::current().check(r.argv, dir, resumerules::Process::live(r.pid, shell));
}

QString TerminalWidget::resumeDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/resume");
}

void TerminalWidget::resumeProgram(const QStringList &command, const QString &cwd)
{
    if (command.isEmpty() || !m_terminal || isExternal())
        return;
    const QString dir = resumeDirectory();
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QByteArray name(12, Qt::Uninitialized);
    for (char &c : name)
        c = "0123456789abcdef"[QRandomGenerator::system()->bounded(16)];
    const QString file = dir + QLatin1Char('/') + QString::fromLatin1(name) + QStringLiteral(".json");
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("argv"), QJsonArray::fromStringList(command)},
                                      {QStringLiteral("cwd"), cwd}}).toJson(QJsonDocument::Compact));
    f.close();

    const auto quote = [](QString w) {
        return QLatin1Char('\'') + w.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
    };
    // A leading space keeps it out of history, in shells that do that.
    const QString line = QLatin1Char(' ') + quote(QCoreApplication::applicationFilePath())
                         + QStringLiteral(" --resume-helper ") + quote(file) + QLatin1Char('\r');
    // Ready: a prompt (OSC 133), or output that has gone quiet, or 5 s.
    auto *poll = new QTimer(this);
    auto prompted = std::make_shared<bool>(false);
    auto lastSeq = std::make_shared<quint64>(m_outputSeq);
    auto quietFor = std::make_shared<int>(0);
    auto waited = std::make_shared<int>(0);
    connect(m_terminal, &QTermWidget::promptStarted, poll, [prompted]() { *prompted = true; });
    connect(poll, &QTimer::timeout, this, [=]() {
        *waited += 50;
        if (m_outputSeq != *lastSeq) {
            *lastSeq = m_outputSeq;
            *quietFor = 0;
        } else if (m_outputSeq > 0) {
            *quietFor += 50;
        }
        if (*prompted || *quietFor >= 400 || *waited >= 5000) {
            poll->stop();
            poll->deleteLater();
            sendText(line);
        }
    });
    poll->start(50);
}

void TerminalWidget::programEnded()
{
    m_statusWatch->stop();
    m_status->programEnded();
}

void TerminalWidget::watchProgramStatusOwner()
{
    emit programStatusReported();
    // Held while the program that sent it is in the foreground. (A tmux
    // pane has no pty here to look at: its TmuxSession says when.)
    if (isExternal())
        return;
    m_statusOwner = statusOwner();
    // Sent by the shell itself (a function at the prompt, or between the
    // commands it runs): held until its next prompt, which a shell marking
    // its prompts says; for one that doesn't, until a command has run and
    // it's back.
    m_statusFromShell = !hasRunningProcess();
    m_statusSawCommand = false;
    const auto records = m_status->records();
    const bool transient = std::any_of(records.begin(), records.end(),
        [](const ProgramStatusRecord &r) { return !r.finished(); });
    if (transient && m_statusOwner != 0 && !(m_statusFromShell && m_promptMarks))
        m_statusWatch->start();
    else
        m_statusWatch->stop();
}

int TerminalWidget::statusOwner() const
{
#ifdef Q_OS_WIN
    // No process groups: what's in the foreground changes with every child
    // a program starts. Whether a program is running (1) or the shell is at
    // its prompt (-1) is what changes when one exits, or one starts.
    return hasRunningProcess() ? 1 : -1;
#else
    return m_terminal->getForegroundProcessId();
#endif
}

void TerminalWidget::checkProgramStatusOwner()
{
    if (!m_statusFromShell) {
        if (statusOwner() != m_statusOwner)
            programEnded();
        return;
    }
    if (hasRunningProcess())
        m_statusSawCommand = true;
    else if (m_statusSawCommand)
        programEnded();
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

QUrl TerminalWidget::linkAt(const QPoint &globalPos) const
{
    Konsole::TerminalDisplay *view = display();
    if (!view)
        return QUrl();
    const auto *spot = dynamic_cast<const Konsole::RegExpFilter::HotSpot *>(
        m_terminal->getHotSpotAt(view->mapFromGlobal(globalPos)));
    if (!spot || spot->type() != Konsole::Filter::HotSpot::Link || spot->capturedTexts().isEmpty())
        return QUrl();
    // As the hot spot opens it: an address without a scheme is a web one.
    QString text = spot->capturedTexts().constFirst();
    if (text.contains(QLatin1Char('@')) && !text.contains(QLatin1String("://")))
        return QUrl();   // an e-mail address
    if (!text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("http://"));
    const QUrl url(text);
    return url.isValid() ? url : QUrl();
}

void TerminalWidget::showContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);

    if (const QUrl link = linkAt(globalPos); link.isValid()) {
        menu.addAction(tr("&Open Link"), this, [this, link]() { emit linkOpened(link); });
        menu.addAction(tr("Copy &Link Address"), this, [link]() {
            QApplication::clipboard()->setText(link.toString());
        });
        menu.addSeparator();
    }

    // Copy is enabled only while there is a selection, asked now: the
    // terminal's copyAvailable() signal is never emitted, so it can't be
    // followed. (A right click leaves the selection be.)
    QAction *copyAct = menu.addAction(tr("&Copy"), m_terminal, &QTermWidget::copyClipboard);
    copyAct->setEnabled(!m_terminal->selectedText(false).isEmpty());
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
