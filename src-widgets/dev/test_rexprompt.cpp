/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 *
 * test-rexprompt: a Rex prompt (rex.ui.input, rex.ui.pick) as PuddleJumper
 * asks it, with the examples from Rex's "Custom UI" page: filtering, keys,
 * ticks and answers; and the command palette, which looks the same. With a directory argument, also saves a picture of each
 * (dark and light) there.
 */

#include "commandpalette.h"
#include "rexprompt.h"

#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPalette>
#include <QStyleFactory>
#include <QTextStream>

namespace {

int failures = 0;
int checks = 0;

bool check(bool ok, const QString &what)
{
    ++checks;
    if (!ok) {
        QTextStream(stderr) << "FAIL: " << what << "\n";
        ++failures;
    }
    return ok;
}

QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(text).object();
}

void key(PaletteDialog &p, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent e(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(p.filterField(), &e);
}

QString answer(const RexPrompt &p)
{
    return QString::fromUtf8(QJsonDocument(p.answer()).toJson(QJsonDocument::Compact));
}

QString shotDir;

void shot(PaletteDialog &p, const QString &name)
{
    if (shotDir.isEmpty())
        return;
    p.show();
    p.activateWindow();
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();
    p.grab().save(QDir(shotDir).filePath(name + QStringLiteral(".png")));
    p.hide();
}

const char *kProjects = R"({"kind": "pick", "title": "Open project", "placeholder": "Filter projects", "items": [
    {"id": "/Users/me/Apps/rex", "title": "rex", "subtitle": "~/Apps/rex", "icon": "folder"},
    {"id": "/Users/me/Apps/rex-app", "title": "rex-app", "subtitle": "~/Apps/rex-app", "icon": "folder"},
    {"id": "/Users/me/Apps/notes", "title": "notes", "subtitle": "~/Apps/notes", "icon": "document"},
    {"id": "build", "title": "build server", "subtitle": "build.example.ts.net", "icon": "server", "detail": "online"}]})";
const char *kStage = R"({"kind": "pick", "title": "Stage files", "multiple": true, "confirm": "Stage",
    "placeholder": "Filter changed files", "items": [
    {"id": "README.md", "selected": true, "detail": "??", "icon": "document"},
    {"id": "docs/notes.md", "title": "notes.md", "subtitle": "docs", "detail": "??", "icon": "document"},
    {"id": "src/main.go", "title": "main.go", "subtitle": "src", "detail": "M", "icon": "code"}]})";
const char *kFruit = R"({"kind": "pick", "title": "Favourite fruit", "allow_other": true,
    "items": ["Apple", "Banana", "Cherry"]})";
const char *kBranch = R"({"kind": "input", "title": "New branch", "placeholder": "feature/…",
    "value": "feature/", "confirm": "Create branch"})";

void run()
{
    {
        RexPrompt p(json(kProjects));
        check(p.shownTitles() == QStringList({"rex", "rex-app", "notes", "build server"}), QStringLiteral("all items, in order"));
        check(p.currentTitle() == QLatin1String("rex"), QStringLiteral("the first current"));
        shot(p, QStringLiteral("pick"));
        p.setFilter(QStringLiteral("REX -APP"));
        check(p.shownTitles() == QStringList{"rex-app"}, QStringLiteral("every word, any case"));
        p.setFilter(QStringLiteral("ts.net"));
        check(p.shownTitles() == QStringList{"build server"}, QStringLiteral("the subtitle filters too"));
        p.setFilter(QStringLiteral("rex"));
        shot(p, QStringLiteral("pick-filtered"));
        key(p, Qt::Key_Down);
        check(p.currentTitle() == QLatin1String("rex-app"), QStringLiteral("down"));
        key(p, Qt::Key_Return);
        check(p.result() == QDialog::Accepted && answer(p) == QLatin1String(R"({"ids":["/Users/me/Apps/rex-app"]})"),
              QStringLiteral("return chooses: ") + answer(p));
    }
    {
        RexPrompt p(json(kProjects));
        p.setFilter(QStringLiteral("nothing like it"));
        check(p.shownTitles() == QStringList{"No matches"}, QStringLiteral("no matches"));
        shot(p, QStringLiteral("pick-empty"));
        key(p, Qt::Key_Return);
        check(p.result() != QDialog::Accepted, QStringLiteral("return on no match: nothing"));
    }
    {
        RexPrompt p(json(kStage));
        check(p.tickedIds() == QStringList{"README.md"}, QStringLiteral("selected starts ticked"));
        key(p, Qt::Key_Down);
        key(p, Qt::Key_Return);
        check(p.tickedIds() == QStringList({"README.md", "docs/notes.md"}) && p.result() != QDialog::Accepted,
              QStringLiteral("return ticks"));
        shot(p, QStringLiteral("multiple"));
        key(p, Qt::Key_Return);
        check(p.tickedIds() == QStringList{"README.md"}, QStringLiteral("and unticks"));
        key(p, Qt::Key_Down);
        key(p, Qt::Key_Return);
        key(p, Qt::Key_Return, Qt::ControlModifier);
        check(answer(p) == QLatin1String(R"({"ids":["README.md","src/main.go"]})"),
              QStringLiteral("cmd+return confirms, in the items' order: ") + answer(p));
    }
    {
        RexPrompt p(json(kFruit));
        p.setFilter(QStringLiteral("Durian"));
        check(p.shownTitles() == QStringList{QStringLiteral("Use “Durian”")}, QStringLiteral("allow_other's row"));
        shot(p, QStringLiteral("allow-other"));
        key(p, Qt::Key_Return);
        check(answer(p) == QLatin1String(R"({"text":"Durian"})"), QStringLiteral("typed text answers"));
    }
    {
        RexPrompt p(json(kFruit));
        p.setFilter(QStringLiteral("an"));
        check(p.shownTitles() == QStringList({"Banana", QStringLiteral("Use “an”")}), QStringLiteral("matches, then Use"));
        key(p, Qt::Key_Return);
        check(answer(p) == QLatin1String(R"({"ids":["Banana"]})"), QStringLiteral("a match still first"));
    }
    {
        RexPrompt p(json(R"({"kind": "pick", "title": "Pick a fruit", "items": ["Apple", "Banana", "Cherry"]})"));
        p.setFilter(QStringLiteral("Durian"));
        check(p.shownTitles() == QStringList{"No matches"}, QStringLiteral("no allow_other: no Use row"));
    }
    {
        RexPrompt p(json(kBranch));
        check(p.filterField()->text() == QLatin1String("feature/") && p.shownTitles() == QStringList{"Create branch"},
              QStringLiteral("input: value, the confirm row"));
        p.setFilter(QStringLiteral("feature/palette"));
        shot(p, QStringLiteral("input"));
        if (!shotDir.isEmpty())
            check(!p.filterField()->hasSelectedText(), QStringLiteral("the value isn't selected"));
        key(p, Qt::Key_Return);
        check(answer(p) == QLatin1String(R"({"text":"feature/palette"})"), QStringLiteral("input answers"));
    }
    {
        RexPrompt p(json(R"({"kind": "input", "title": "Your name"})"));
        check(p.shownTitles() == QStringList{QStringLiteral("Use “”")}, QStringLiteral("input without confirm"));
        key(p, Qt::Key_Return);
        check(answer(p) == QLatin1String(R"({"text":""})"), QStringLiteral("empty text is an answer"));
    }
    {
        RexPrompt p(json(R"({"kind": "pick", "title": "Nothing", "items": []})"));
        check(p.shownTitles() == QStringList{"Nothing to choose from"}, QStringLiteral("no items"));
    }
    check(RexPrompt::iconNames().size() == 16, QStringLiteral("Rex's sixteen icons"));

    // The command palette.
    {
        QString ran;
        const auto entry = [&ran](const char *title, const char *category, const char *name, const char *keys,
                                  const char *keywords = "") {
            const QString n = QString::fromLatin1(name);
            return CommandPalette::Entry{QString::fromUtf8(title), QString::fromLatin1(category), n,
                                         QString::fromLatin1(keywords), QString::fromUtf8(keys), [&ran, n]() { ran = n; }};
        };
        QWidget host;
        auto *p = new CommandPalette({entry("New Tab", "Tabs", "client.tab.new", "Ctrl+Shift+T"),
                                      entry("Close Tab", "Tabs", "client.tab.close", "Ctrl+Shift+W"),
                                      entry("Split Right", "Panes", "pane.split.right", "Ctrl+Shift+D"),
                                      entry("Split Down", "Panes", "pane.split.down", "Ctrl+Shift+E"),
                                      entry("Copy", "Edit", "pane.copy", "Ctrl+Shift+C"),
                                      entry("Paste", "Edit", "pane.paste", "Ctrl+Shift+V"),
                                      entry("Clear Terminal", "Terminal", "com.superlogical.terminal.clear", "",
                                            "clear screen scrollback"),
                                      entry("Git: New Branch…", "Git", "new_branch", "Alt+Ctrl+B")},
                                     &host);
        check(p->shown().first() == QLatin1String("pane.copy"), QStringLiteral("palette: by category, then title"));
        shot(*p, QStringLiteral("palette"));
        p->setFilter(QStringLiteral("split"));
        check(p->shown() == QStringList({"pane.split.down", "pane.split.right"}), QStringLiteral("palette: filtered"));
        shot(*p, QStringLiteral("palette-filtered"));
        p->setFilter(QStringLiteral("scrollback"));
        check(p->shown() == QStringList{"com.superlogical.terminal.clear"}, QStringLiteral("palette: keywords"));
        p->setFilter(QStringLiteral("zzz"));
        check(p->shown().isEmpty() && p->shownTitles() == QStringList{"No matching actions"},
              QStringLiteral("palette: nothing"));
        p->setFilter(QStringLiteral("tab"));
        key(*p, Qt::Key_Down);
        key(*p, Qt::Key_Return);
        QApplication::processEvents();
        check(ran == QLatin1String("client.tab.new"), QStringLiteral("palette: return runs, after closing: ") + ran);
    }
}

void setDark(bool dark)
{
    QPalette p;
    if (dark) {
        p.setColor(QPalette::Window, QColor(0x24, 0x26, 0x2b));
        p.setColor(QPalette::WindowText, QColor(0xe6, 0xe6, 0xe6));
        p.setColor(QPalette::Base, QColor(0x1c, 0x1d, 0x21));
        p.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
        p.setColor(QPalette::Button, QColor(0x33, 0x35, 0x3b));
        p.setColor(QPalette::ButtonText, QColor(0xe6, 0xe6, 0xe6));
        p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x82, 0x88));
    } else {
        p.setColor(QPalette::Window, QColor(0xf4, 0xf4, 0xf5));
        p.setColor(QPalette::WindowText, QColor(0x1d, 0x1d, 0x1f));
        p.setColor(QPalette::Base, Qt::white);
        p.setColor(QPalette::Text, QColor(0x1d, 0x1d, 0x1f));
        p.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0xa0));
    }
    p.setColor(QPalette::Highlight, QColor(0x3d, 0x7e, 0xf0));
    p.setColor(QPalette::Accent, QColor(0x3d, 0x7e, 0xf0));
    p.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(p);
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    if (argc > 1) {
        shotDir = QString::fromLocal8Bit(argv[1]);
        QDir().mkpath(shotDir);
    }
    const QString base = shotDir;
    setDark(true);
    if (!base.isEmpty())
        shotDir = base + QStringLiteral("/dark");
    QDir().mkpath(shotDir);
    run();
    if (!base.isEmpty()) {
        setDark(false);
        shotDir = base + QStringLiteral("/light");
        QDir().mkpath(shotDir);
        run();
    }
    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures ? 1 : 0;
}
