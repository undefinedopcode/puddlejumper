/*
 * Debug probe — NOT installed, NOT part of the app.
 *
 * Instantiates SettingsDialog offscreen, switches to the Tabs & Window page,
 * dumps the object tree + sizes of the output-monitoring group, and saves a
 * rendered snapshot to probe-settingsdialog.png.
 */

#include "../profiles.h"
#include "../settingsdialog.h"
#include "../settings.h"
#include "../serverstore.h"

#include <QStandardPaths>

#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

int main(int argc, char *argv[])
{
    // probe-settingsdialog servers OUT.png: the Servers page with a few
    // entries, to look at (test-mode settings, the real ones left alone).
    // probe-settingsdialog terminal OUT.png: the Terminal page (its Rex
    // group among them).
    if (argc > 2 && (QByteArray(argv[1]) == "servers" || QByteArray(argv[1]) == "terminal"))
        QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    if (argc > 2 && QByteArray(argv[1]) == "terminal") {
        Settings settings;
        settings.setRexScriptsFile(QStringLiteral("/home/me/.config/puddlejumper/keys.lua"));
        ProfileStore profiles;
        SettingsDialog dlg(settings, profiles);
        dlg.resize(860, 760);
        dlg.showSection(SettingsDialog::Terminal);
        for (int i = 0; i < 5; ++i)
            app.processEvents();
        dlg.grab().save(QString::fromLocal8Bit(argv[2]));
        settings.setRexScriptsFile(QString());
        return 0;
    }

    if (argc > 2 && QByteArray(argv[1]) == "servers") {
        Settings settings;
        ProfileStore profiles;
        ServerStore servers;
        for (const SavedServer &s : servers.servers())
            servers.remove(s.id);
        SavedServer a;
        a.kind = SavedServer::Rex;
        a.name = QStringLiteral("Home Rex");
        a.address = QStringLiteral("http://127.0.0.1:51433");
        servers.save(a);
        SavedServer b;
        b.kind = SavedServer::Tmux;
        b.address = QStringLiteral("me@buildbox");
        b.session = QStringLiteral("work");
        servers.save(b);
        SavedServer c;
        c.kind = SavedServer::Vnc;
        c.name = QStringLiteral("Mac VM");
        c.address = QStringLiteral("localhost:5911");
        c.adaptive = true;
        servers.save(c);
        SavedServer d;
        d.kind = SavedServer::Web;
        d.name = QStringLiteral("Dev server");
        d.address = QStringLiteral("localhost:3000");
        servers.save(d);
        SettingsDialog dlg(settings, profiles, nullptr, &servers);
        dlg.resize(860, 560);
        dlg.showSection(SettingsDialog::Servers);
        if (argc > 3) {
            auto *list = dlg.findChildren<QListWidget *>().last();
            list->setCurrentRow(QByteArray(argv[3]).toInt());
        }
        for (int i = 0; i < 5; ++i)
            app.processEvents();
        dlg.grab().save(QString::fromLocal8Bit(argv[2]));
        for (const SavedServer &s : servers.servers())
            servers.remove(s.id);
        return 0;
    }

    Settings settings;
    ProfileStore profiles;
    SettingsDialog dlg(settings, profiles);
    dlg.showSection(SettingsDialog::Window);
    dlg.show();

    // Give layouts + polish a turn to propagate.
    app.processEvents();
    app.processEvents();

    // Find the group by title among SettingsDialog children.
    auto title = QStringLiteral("Watch a Terminal");
    QList<QGroupBox *> groups = dlg.findChildren<QGroupBox *>();
    QGroupBox *watch = nullptr;
    for (auto *g : groups) {
        if (g->title() == title) {
            watch = g;
            break;
        }
    }

    if (!watch) {
        fprintf(stderr, "PROBE: group box %s NOT FOUND\n", qPrintable(title));
        return 2;
    }

    fprintf(stderr, "PROBE: found group; size=%dx%d sizeHint=%dx%d children:\n",
            watch->size().width(), watch->size().height(),
            watch->sizeHint().width(), watch->sizeHint().height());

    for (QObject *c : watch->children()) {
        auto *w = qobject_cast<QWidget *>(c);
        if (!w)
            continue;
        auto *cb = qobject_cast<QCheckBox *>(c);
        auto *sb = qobject_cast<QSpinBox *>(c);
        fprintf(stderr,
                "  child class=%-16s size=%3dx%-3d visible=%d"
                " %s%s%s\n",
                c->metaObject()->className(),
                w->size().width(), w->size().height(),
                w->isVisible(),
                cb ? qPrintable(QStringLiteral("text=\"") + cb->text() + "\" ") : "",
                sb ? qPrintable(QStringLiteral("value=") + QString::number(sb->value()) + " ") : "",
                w->isEnabled() ? "" : "[disabled]");
    }

    // Snapshot the whole dialog.
    const QPixmap pm = dlg.grab();
    if (!pm.save(QStringLiteral("probe-settingsdialog.png")))
        fprintf(stderr, "PROBE: grab save FAILED\n");
    else
        fprintf(stderr, "PROBE: saved %dx%d to probe-settingsdialog.png\n",
                pm.width(), pm.height());

    // Diagnostics: what height does the page need vs how tall is the window?
    if (auto *pages = dlg.findChild<QStackedWidget *>(QString())) {
        fprintf(stderr, "PROBE: stacked sizeHint=%dx%d  current page hint=%dx%d  window=%dx%d\n",
                pages->sizeHint().width(), pages->sizeHint().height(),
                pages->currentWidget()->sizeHint().width(),
                pages->currentWidget()->sizeHint().height(),
                dlg.size().width(), dlg.size().height());
    }
    return 0;
}
