/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "colorscheme.h"
#include "disclaim.h"
#include "formername.h"
#include "programresume.h"
#include "settings.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QStyle>
#include <QStyleFactory>
#include <QUrl>

#include <qtermwidget.h>

#include <cstring>

#ifdef Q_OS_UNIX
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int quitPipe[2] = {-1, -1};

// Only a write: all that's safe in a signal handler. The rest is done when
// the event loop reads it.
void onQuitSignal(int)
{
    const char c = 1;
    [[maybe_unused]] const auto n = ::write(quitPipe[0], &c, 1);
}

/*
 * SIGTERM and SIGHUP (a logout, a kill, the terminal going) save what's open,
 * as closing the window does, then quit: what was open is there next time,
 * Rex and tmux sessions included. No asking: nobody is there to answer.
 */
void quitOnSignals(MainWindow *window)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, quitPipe) != 0)
        return;
    auto *notifier = new QSocketNotifier(quitPipe[1], QSocketNotifier::Read, window);
    QObject::connect(notifier, &QSocketNotifier::activated, window, [window, notifier]() {
        char c;
        [[maybe_unused]] const auto n = ::read(quitPipe[1], &c, 1);
        notifier->setEnabled(false);
        window->saveForQuit();
        QApplication::quit();
    });
    struct sigaction sa = {};
    sa.sa_handler = onQuitSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
}
} // namespace
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_UNIX
    if (argc == 3 && strcmp(argv[1], "--resume-helper") == 0)
        return ProgramResume::runHelper(argv[2]);
#endif
    runDisclaimedIfAsked(argc, argv);   // macOS: started to become a command (see disclaim.h)

#ifdef PUDDLEJUMPER_WEB
    // Web tabs' pages are drawn with OpenGL contexts shared with the window's.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif
    QApplication app(argc, argv);
    // Must match the QML build so both share one config file.
    app.setOrganizationName(QStringLiteral("puddlejumper"));
    app.setApplicationName(QStringLiteral("puddlejumper"));
    app.setApplicationDisplayName(QStringLiteral("PuddleJumper"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));
    app.setDesktopFileName(QStringLiteral("puddlejumper"));
    adoptFormerName();   // superterm's settings, the first time

    /*
     * The shells are ours, not those of the terminal puddlejumper may have been
     * started from: drop that one's identity, which they would inherit. On
     * macOS, TERM_PROGRAM=Apple_Terminal makes /etc/zshrc load Terminal's
     * session save/restore, and an inherited TERM_SESSION_ID has every shell
     * share (and delete) one session file.
     */
    for (const char *name : {"TERM_SESSION_ID", "ITERM_SESSION_ID", "TERM_PROGRAM_VERSION",
                             "KONSOLE_DBUS_SESSION", "KONSOLE_VERSION", "VTE_VERSION",
                             "WT_SESSION"})
        qunsetenv(name);
    // A resume token is the terminal's for its own first process, never
    // passed on (OSC 7577): each local tab gets its own.
    qunsetenv("OSC7577_TOKEN");
    qputenv("TERM_PROGRAM", "puddlejumper");
    qputenv("TERM_PROGRAM_VERSION", app.applicationVersion().toUtf8());

    // Started from a launcher (the Finder, the Dock, a desktop entry), the
    // working directory is /: shells start at home instead.
    if (QDir::currentPath() == QDir::rootPath())
        QDir::setCurrent(QDir::homePath());

#ifdef Q_OS_MACOS
    // An app opened from the Finder has no locale in its environment, and
    // shells without one fall back to ASCII. Terminal sets one from the
    // system's language and region; so do we, unless one is set.
    if (qEnvironmentVariableIsEmpty("LANG") && qEnvironmentVariableIsEmpty("LC_ALL")
        && qEnvironmentVariableIsEmpty("LC_CTYPE")) {
        QString locale = QLocale::system().name() + QStringLiteral(".UTF-8");
        if (!QFile::exists(QStringLiteral("/usr/share/locale/") + locale))
            locale = QStringLiteral("en_US.UTF-8");
        qputenv("LANG", locale.toUtf8());
    }
    // Nor the PATH a login shell gets: tmux, which the app runs itself, is
    // Homebrew's (or MacPorts') more often than not. The usual places, after
    // what's there.
    {
        QStringList path = qEnvironmentVariable("PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const char *dir : {"/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin",
                                "/usr/local/sbin", "/opt/local/bin", "/opt/local/sbin"}) {
            const QString d = QString::fromLatin1(dir);
            if (!path.contains(d) && QFileInfo(d).isDir())
                path.append(d);
        }
        qputenv("PATH", QFile::encodeName(path.join(QLatin1Char(':'))));
    }
#ifdef PUDDLEJUMPER_WEB
    // Qt WebEngine finds its helper and data in the app only when Qt is built
    // as frameworks; conda's isn't, so the app carries them where
    // build-app.sh puts them, and says so. (Without them it aborts.)
    {
        const QDir contents(app.applicationDirPath() + QStringLiteral("/.."));
        const auto point = [&contents](const char *name, const QString &path) {
            if (qEnvironmentVariableIsEmpty(name) && QFile::exists(contents.filePath(path)))
                qputenv(name, QFile::encodeName(contents.absoluteFilePath(path)));
        };
        point("QTWEBENGINEPROCESS_PATH", QStringLiteral("MacOS/QtWebEngineProcess"));
        point("QTWEBENGINE_RESOURCES_PATH", QStringLiteral("Resources/qtwebengine"));
        point("QTWEBENGINE_LOCALES_PATH", QStringLiteral("Resources/qtwebengine/qtwebengine_locales"));
    }
#endif
#endif

    /*
     * Register the user scheme directory before anything reads the scheme
     * list. qtermwidget caches its scheme list the first time it is asked, so
     * a directory registered later in the run would be missed until restart.
     */
    const QString userSchemes = ColorSchemeFile::userSchemeDir();
    if (!userSchemes.isEmpty()) {
        QDir().mkpath(userSchemes);
        QTermWidget::addCustomColorSchemeDir(userSchemes);

        /*
         * Remove stand-in copies left behind by a previous session. They exist
         * only so an in-place scheme edit can bypass qtermwidget's basename
         * cache within one run, and are deleted as soon as they are superseded;
         * anything still here is a leftover from a crash or a kill.
         */
        Settings::removeLiveSchemeFiles();
    }

    // Window/taskbar icon. Embedded via qt_add_resources so it is present
    // whether the app runs from the build tree or an install prefix. Multi-size
    // from the one scalable source keeps small sizes sharp.
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        appIcon.addFile(QStringLiteral(":/icons/puddlejumper.svg"),
                        QSize(size, size));
    }
    QApplication::setWindowIcon(appIcon);

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption styleOpt(
        QStringLiteral("style"),
        QStringLiteral("Force a widget style (e.g. Fusion, Breeze). Default is "
                       "the platform style."),
        QStringLiteral("name"));
    parser.addOption(styleOpt);
    QCommandLineOption vncOpt(
        QStringLiteral("vnc"),
        QStringLiteral("Open a VNC tab on this address (host, host:port or host:display)."),
        QStringLiteral("address"));
    parser.addOption(vncOpt);
    QCommandLineOption rdpOpt(
        QStringLiteral("rdp"),
        QStringLiteral("Open an RDP tab on this address (host or host:port)."),
        QStringLiteral("address"));
    parser.addOption(rdpOpt);
    QCommandLineOption webOpt(
        QStringLiteral("web"),
        QStringLiteral("Open a web tab on this address (localhost:3000, a URL)."),
        QStringLiteral("address"));
    parser.addOption(webOpt);
    parser.addPositionalArgument(
        QStringLiteral("workspace"),
        QStringLiteral("A workspace to open: a folder (its .puddlejumper.yml or .smug.yml, "
                       "or a workspace for the folder), a workspace file, or the name of "
                       "one in ~/.config/puddlejumper/workspaces or ~/.config/smug."),
        QStringLiteral("[workspace]"));
    parser.process(app);

    if (parser.isSet(styleOpt)) {
        const QString name = parser.value(styleOpt);
        if (auto *s = QStyleFactory::create(name)) {
            QApplication::setStyle(s);
            qInfo() << "forced widget style:" << name;
        } else {
            qWarning() << "unknown style" << name
                       << "- available:" << QStyleFactory::keys();
        }
    }
    MainWindow window;
    window.show();
#ifdef Q_OS_UNIX
    quitOnSignals(&window);
#endif
    if (parser.isSet(vncOpt))
        window.openVnc(parser.value(vncOpt));
    if (parser.isSet(rdpOpt))
        window.openRdp(parser.value(rdpOpt));
    if (parser.isSet(webOpt))
        window.openWeb(QUrl::fromUserInput(parser.value(webOpt)));
    if (const QStringList args = parser.positionalArguments(); !args.isEmpty()) {
        // Once the window is up, so dialogs (variables, trust) have a parent.
        const QString target = args.first();
        QTimer::singleShot(0, &window, [&window, target]() { window.openWorkspaceTarget(target); });
    }
    return app.exec();
}
