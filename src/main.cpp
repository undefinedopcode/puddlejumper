/*
 * superterm — a modern terminal built on Qt Quick and QMLTermWidget.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QFontDatabase>
#include <QIcon>
#include <QDir>
#include <QDebug>

#include "settings_store.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("superterm"));
    app.setApplicationDisplayName(QStringLiteral("SuperTerm"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("superterm"));
    app.setDesktopFileName(QStringLiteral("superterm"));

    // A custom QML style would need to ship its own controls; Fusion is the
    // most unobtrusive built-in and looks the same everywhere.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    // QMLTermWidget is installed as a system QML module. Put its directory on
    // the import path so the engine can find it when run from a build tree.
#ifdef SUPERTERM_QMLTERMWIDGET_IMPORT_DIR
    QByteArray importPath = qgetenv("QML2_IMPORT_PATH");
    const QByteArray ours = SUPERTERM_QMLTERMWIDGET_IMPORT_DIR;
    if (!importPath.split(QDir::listSeparator().toLatin1()).contains(ours)) {
        if (!importPath.isEmpty())
            importPath.prepend(QDir::listSeparator().toLatin1());
        importPath.prepend(ours);
        qputenv("QML2_IMPORT_PATH", importPath);
    }
#endif

    // Prefer a real monospace face: QMLTermWidget warns (and miscalculates
    // cell geometry) when handed a variable-width font.
    const QStringList candidates = {
        QStringLiteral("Hack"),
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Fira Code"),
        QStringLiteral("Iosevka Fixed"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Liberation Mono"),
        QStringLiteral("Noto Sans Mono"),
        QStringLiteral("Menlo"),
        QStringLiteral("Consolas"),
        QStringLiteral("monospace"),
    };
    const QStringList families = QFontDatabase::families();
    QString mono = QStringLiteral("monospace");
    for (const QString &candidate : candidates) {
        if (families.contains(candidate, Qt::CaseInsensitive)) {
            mono = candidate;
            break;
        }
    }

    // Every monospace family, for the font picker in Settings.
    QStringList monoFamilies;
    for (const QString &family : families) {
        if (QFontDatabase::isFixedPitch(family))
            monoFamilies.append(family);
    }
    monoFamilies.sort(Qt::CaseInsensitive);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("defaultMonoFont"), mono);
    engine.rootContext()->setContextProperty(QStringLiteral("monoFontFamilies"), monoFamilies);

    SettingsStore settingsStore;
    engine.rootContext()->setContextProperty(QStringLiteral("settingsStore"), &settingsStore);

    // Optional shell override, useful for testing:
    //   SUPERTERM_SHELL=/bin/fish SUPERTERM_SHELL_ARGS="-l" superterm
    engine.rootContext()->setContextProperty(QStringLiteral("envShell"),
        QString::fromLocal8Bit(qgetenv("SUPERTERM_SHELL")));
    engine.rootContext()->setContextProperty(QStringLiteral("envShellArgs"),
        QString::fromLocal8Bit(qgetenv("SUPERTERM_SHELL_ARGS")).split(
            QLatin1Char(' '), Qt::SkipEmptyParts));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("Superterm", "Main");

    return app.exec();
}
