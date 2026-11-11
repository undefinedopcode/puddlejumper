/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Must match the QML build so both share one config file.
    app.setOrganizationName(QStringLiteral("superterm"));
    app.setApplicationName(QStringLiteral("superterm"));
    app.setApplicationDisplayName(QStringLiteral("SuperTerm"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));
    app.setDesktopFileName(QStringLiteral("superterm"));

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
