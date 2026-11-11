/*
 * Debug probe — NOT installed, NOT part of the app.
 *
 * Instantiates SettingsDialog offscreen, switches to the Scrolling page,
 * dumps the object tree + sizes of the output-monitoring group, and saves a
 * rendered snapshot to probe-settingsdialog.png.
 */

#include "../profiles.h"
#include "../settingsdialog.h"
#include "../settings.h"

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
    QApplication app(argc, argv);

    Settings settings;
    ProfileStore profiles;
    SettingsDialog dlg(settings, profiles);
    dlg.showSection(SettingsDialog::Scrolling);
    dlg.show();

    // Give layouts + polish a turn to propagate.
    app.processEvents();
    app.processEvents();

    // Find the group by title among SettingsDialog children.
    auto title = QStringLiteral("Watch a terminal");
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
