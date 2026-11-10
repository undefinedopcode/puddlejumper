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
#include <QFileInfo>
#include <QLibraryInfo>

#include "settings_store.h"

// Qt Quick Controls resolves a style by import name, so "installed" simply
// means a matching qmldir exists somewhere on the QML import path.
static bool qmlModuleAvailable(const QString &relativeQmldir)
{
    // Escapes for testing: treat a style as unavailable without uninstalling
    // it, so the fallback chain can be exercised on a machine that has all of
    // them. Comma-separated, e.g. SUPERTERM_HIDE_STYLES=org.kde.union
    const QString hidden = QString::fromLocal8Bit(qgetenv("SUPERTERM_HIDE_STYLES"));
    if (!hidden.isEmpty()) {
        // Compare module names, translating the dotted form to its path form
        // (org.kde.union -> /org/kde/union/).
        const QString target = relativeQmldir;
        for (const QString &entry : hidden.split(QLatin1Char(','))) {
            QString module = entry.trimmed();
            if (module.isEmpty())
                continue;
            const QString asPath = QLatin1Char('/')
                + QString(module).replace(QLatin1Char('.'), QLatin1Char('/'))
                + QLatin1Char('/');
            if (target.startsWith(asPath))
                return false;
        }
    }

    QStringList roots;
    const QByteArray envPaths = qgetenv("QML2_IMPORT_PATH");
    if (!envPaths.isEmpty())
        roots << QString::fromLocal8Bit(envPaths).split(QDir::listSeparator());
    roots << QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
    roots << QStringLiteral("/usr/lib/qt6/qml")
          << QStringLiteral("/usr/lib64/qt6/qml")
          << QStringLiteral("/usr/local/lib/qt6/qml");

    for (const QString &root : roots) {
        if (!root.isEmpty() && QFileInfo::exists(root + relativeQmldir))
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("superterm"));
    app.setApplicationDisplayName(QStringLiteral("SuperTerm"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("superterm"));
    app.setDesktopFileName(QStringLiteral("superterm"));

    // Controls style: separate from the colour scheme below.
    //
    // On a Plasma session the KDE platform theme supplies the palette to
    // SystemPalette regardless of which Quick style is active, so Fusion
    // reports exactly the same colours as the KDE styles. Preferring Union is
    // therefore about control *look*, not colour: Union is Plasma's own style
    // and covers both QtWidgets and QtQuick.
    //
    // "org.kde.desktop" (qqc2-desktop-style) is deliberately NOT used:
    // version 6.31.0 segfaults the moment a ComboBox is instantiated, it
    // offers no colour advantage here, and Union supersedes it. Set
    // SUPERTERM_QUICK_STYLE=<name> to override.
    const bool isKdeSession =
        qgetenv("XDG_CURRENT_DESKTOP").toUpper().contains("KDE")
        || !qgetenv("KDE_FULL_SESSION").isEmpty();

    QString style = QStringLiteral("Fusion");
    const QByteArray overrideStyle = qgetenv("SUPERTERM_QUICK_STYLE");
    if (!overrideStyle.isEmpty()) {
        style = QString::fromLocal8Bit(overrideStyle);
    } else if (isKdeSession
               && qmlModuleAvailable(QStringLiteral("/org/kde/union/qmldir"))) {
        style = QStringLiteral("org.kde.union");
    }
    // The terminal chrome follows the desktop palette whenever the session
    // actually supplies one — which is what SystemPalette reports.
    const bool useNativeStyle = isKdeSession;
    QQuickStyle::setStyle(style);
    if (qEnvironmentVariableIsSet("SUPERTERM_SHOWSTYLE"))
        qInfo() << "quick style:" << style << "followsSystem:" << useNativeStyle;

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
    // Tells Theme.qml whether the chrome should follow the desktop palette.
    engine.rootContext()->setContextProperty(QStringLiteral("chromeFollowsSystem"),
                                             useNativeStyle);

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
