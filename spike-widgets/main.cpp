/*
 * superterm — Qt Widgets spike entry point.
 *
 * Uses QApplication (not QGuiApplication): widgets need it, and it is also
 * what makes the platform style and its palette available.
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
    app.setApplicationName(QStringLiteral("superterm-widgets"));
    app.setApplicationDisplayName(QStringLiteral("SuperTerm (widgets spike)"));
    app.setOrganizationName(QStringLiteral("superterm"));

    // Optional style override, mostly to compare platform styling against the
    // neutral Fusion look: --style Fusion
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption styleOpt(QStringLiteral("style"),
                                QStringLiteral("Force a widget style (e.g. Fusion, Breeze)."),
                                QStringLiteral("name"));
    parser.addOption(styleOpt);
    parser.process(app);

    if (parser.isSet(styleOpt)) {
        const QString name = parser.value(styleOpt);
        if (auto *s = QStyleFactory::create(name)) {
            QApplication::setStyle(s);
            qInfo() << "forced widget style:" << name;
        } else {
            qWarning() << "unknown style:" << name
                       << "available:" << QStyleFactory::keys();
        }
    } else {
        // Report what the platform gave us -- this is the consistency payoff.
        qInfo() << "platform widget style:" << QApplication::style()->objectName();
    }

    MainWindow window;
    window.show();
    return app.exec();
}
