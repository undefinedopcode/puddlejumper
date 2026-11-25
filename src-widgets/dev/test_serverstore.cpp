/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-serverstore: saved servers through QSettings and back (test-mode
 * settings, so the real ones are left alone).
 */

#include "serverstore.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTextStream>

namespace {
int failures = 0;
int checks = 0;
void check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL: " << what << "\n";
        ++failures;
    }
}
} // namespace

int main(int argc, char *argv[])
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("puddlejumper-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test-serverstore"));
    QSettings().clear();

    {
        ServerStore store;
        check(store.servers().isEmpty(), QStringLiteral("empty to start"));
        QSignalSpy changed(&store, &ServerStore::changed);
        SavedServer rex;
        rex.kind = SavedServer::Rex;
        rex.name = QStringLiteral("home");
        rex.address = QStringLiteral("127.0.0.1:51433");
        const QString rexId = store.save(rex);
        SavedServer tmux;
        tmux.kind = SavedServer::Tmux;
        tmux.address = QStringLiteral("me@box");
        tmux.session = QStringLiteral("work");
        store.save(tmux);
        SavedServer vnc;
        vnc.kind = SavedServer::Vnc;
        vnc.name = QStringLiteral("mac");
        vnc.address = QStringLiteral("localhost:5911");
        vnc.user = QStringLiteral("someone");
        vnc.reducedColour = true;
        vnc.adaptive = true;
        const QString vncId = store.save(vnc);
        check(changed.count() == 3, QStringLiteral("changed on each save"));
        check(!rexId.isEmpty() && rexId != vncId, QStringLiteral("ids given"));
        check(store.servers().at(1).title() == QLatin1String("me@box"), QStringLiteral("title falls back to the address"));

        // Rex addresses match whatever way they're written.
        check(store.rexServer(QStringLiteral("http://127.0.0.1:51433/")).id == rexId, QStringLiteral("rex match"));
        check(store.rexServer(QStringLiteral("http://127.0.0.1:9")).id.isEmpty(), QStringLiteral("rex no match"));

        // Replace, move, remove.
        SavedServer edited = store.server(rexId);
        edited.name = QStringLiteral("home server");
        store.save(edited);
        check(store.servers().size() == 3 && store.servers().first().name == QLatin1String("home server"),
              QStringLiteral("replaced in place"));
        store.move(vncId, -1);
        check(store.servers().at(1).id == vncId, QStringLiteral("moved up"));
        store.move(rexId, -1);
        check(store.servers().first().id == rexId, QStringLiteral("can't move past the top"));
    }
    {
        // Read back by a new store.
        ServerStore store;
        const QList<SavedServer> list = store.servers();
        check(list.size() == 3, QStringLiteral("three read back"));
        if (list.size() == 3) {
            check(list.at(0).kind == SavedServer::Rex && list.at(0).name == QLatin1String("home server"),
                  QStringLiteral("rex back"));
            check(list.at(1).kind == SavedServer::Vnc && list.at(1).user == QLatin1String("someone")
                      && list.at(1).reducedColour && list.at(1).adaptive,
                  QStringLiteral("vnc back with its options"));
            check(list.at(2).kind == SavedServer::Tmux && list.at(2).session == QLatin1String("work"),
                  QStringLiteral("tmux back with its session"));
            store.remove(list.at(1).id);
        }
    }
    {
        ServerStore store;
        check(store.servers().size() == 2, QStringLiteral("removed for good"));
        // An entry of a kind this build doesn't know is skipped, not fatal.
        QSettings().setValue(QStringLiteral("servers/list"),
                             QStringLiteral("[{\"kind\":\"gopher\",\"address\":\"x\"},{\"kind\":\"web\",\"address\":\"localhost:3000\"}]"));
        ServerStore again;
        check(again.servers().size() == 1 && again.servers().first().kind == SavedServer::Web,
              QStringLiteral("unknown kinds skipped"));
    }
    QSettings().clear();

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
