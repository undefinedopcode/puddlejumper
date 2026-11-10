/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
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
