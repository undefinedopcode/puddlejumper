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

// The KDE Qt Quick Controls style ships with qqc2-desktop-style. Probe for it
// before asking Qt to use it, so a KDE session without that package falls back
// to Fusion rather than failing to construct controls at startup.
static bool kdeQuickStyleAvailable()
{
    static const QString kRelative = QStringLiteral("/org/kde/desktop/qmldir");

    QStringList roots;
    const QByteArray envPaths = qgetenv("QML2_IMPORT_PATH");
    if (!envPaths.isEmpty())
        roots << QString::fromLocal8Bit(envPaths).split(QDir::listSeparator());
    roots << QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
    roots << QStringLiteral("/usr/lib/qt6/qml")
          << QStringLiteral("/usr/lib64/qt6/qml")
          << QStringLiteral("/usr/local/lib/qt6/qml");

    for (const QString &root : roots) {
        if (!root.isEmpty() && QFileInfo::exists(root + kRelative))
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

    // Under KDE, prefer the native QML style (org.kde.desktop, from
    // qqc2-desktop-style). It draws controls with the user's Plasma widget
    // style and exposes their colour scheme through the QML palette, so the
    // chrome matches the rest of the desktop. Elsewhere Fusion is the most
    // unobtrusive built-in and looks the same everywhere.
    const bool isKdeSession =
        qgetenv("XDG_CURRENT_DESKTOP").toUpper().contains("KDE")
        || !qgetenv("KDE_FULL_SESSION").isEmpty();
    const bool useNativeStyle = isKdeSession && kdeQuickStyleAvailable();
    if (useNativeStyle)
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    else
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
