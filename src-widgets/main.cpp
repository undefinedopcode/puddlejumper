/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "colorscheme.h"
#include "settings.h"
#include "vncview.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>

#include <qtermwidget.h>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Must match the QML build so both share one config file.
    app.setOrganizationName(QStringLiteral("superterm"));
    app.setApplicationName(QStringLiteral("superterm"));
    app.setApplicationDisplayName(QStringLiteral("SuperTerm"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));
    app.setDesktopFileName(QStringLiteral("superterm"));

    /*
     * The shells are ours, not those of the terminal superterm may have been
     * started from: drop that one's identity, which they would inherit. On
     * macOS, TERM_PROGRAM=Apple_Terminal makes /etc/zshrc load Terminal's
     * session save/restore, and an inherited TERM_SESSION_ID has every shell
     * share (and delete) one session file.
     */
    for (const char *name : {"TERM_SESSION_ID", "ITERM_SESSION_ID", "TERM_PROGRAM_VERSION",
                             "KONSOLE_DBUS_SESSION", "KONSOLE_VERSION", "VTE_VERSION",
                             "WT_SESSION"})
        qunsetenv(name);
    qputenv("TERM_PROGRAM", "superterm");
    qputenv("TERM_PROGRAM_VERSION", app.applicationVersion().toUtf8());

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
        appIcon.addFile(QStringLiteral(":/icons/superterm.svg"),
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
        QStringLiteral("Also open a VNC viewer on this address (host, host:port or "
                       "host:display)."),
        QStringLiteral("address"));
    parser.addOption(vncOpt);
    parser.addPositionalArgument(
        QStringLiteral("workspace"),
        QStringLiteral("A workspace to open: a folder (its .superterm.yml or .smug.yml, "
                       "or a workspace for the folder), a workspace file, or the name of "
                       "one in ~/.config/superterm/workspaces or ~/.config/smug."),
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
    if (parser.isSet(vncOpt)) {
        QString host;
        quint16 port;
        if (VncView::parseAddress(parser.value(vncOpt), &host, &port))
            VncView::openWindow(host, port);
        else
            qWarning() << "not a VNC address:" << parser.value(vncOpt);
    }
    if (const QStringList args = parser.positionalArguments(); !args.isEmpty()) {
        // Once the window is up, so dialogs (variables, trust) have a parent.
        const QString target = args.first();
        QTimer::singleShot(0, &window, [&window, target]() { window.openWorkspaceTarget(target); });
    }
    return app.exec();
}
