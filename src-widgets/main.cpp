/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include "colorscheme.h"
#include "settings.h"

#include <QApplication>
#include <QCommandLineParser>
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
    return app.exec();
}
