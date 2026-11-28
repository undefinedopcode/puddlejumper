/*
 * test-systemscheme — the System colour scheme (ColorSchemeFile::
 * systemScheme()): made from a palette's base, text and accent, every
 * colour readable on its background, light and dark desktops alike, and a
 * new file when the desktop's colours change. Prints each failure; exits 1
 * if there was any.
 *
 * Run: ./build/test-systemscheme
 */

#include "colorscheme.h"

#include <QApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    if (!ok) {
        QTextStream(stderr) << "FAIL " << what << "\n";
        ++failures;
    }
}

QPalette palette(const char *base, const char *text, const char *accent)
{
    QPalette p;
    p.setColor(QPalette::Base, QColor(base));
    p.setColor(QPalette::Text, QColor(text));
    p.setColor(QPalette::Accent, QColor(accent));
    p.setColor(QPalette::Highlight, QColor(accent));
    return p;
}

using S = ColorSchemeFile;

void readable(const char *name, const QPalette &p)
{
    const S s = S::systemScheme(p);
    const QColor bg = s.colors[S::Background];
    const auto ratio = [&](int role) { return S::contrast(s.colors[role], bg); };
    const auto at = [&](int role, double want) {
        check(ratio(role) >= want - 0.01, QStringLiteral("%1: %2 %3 on %4 is %5:1, want %6")
                                              .arg(QLatin1String(name), S::roleLabel(role), s.colors[role].name(),
                                                   bg.name()).arg(ratio(role), 0, 'f', 2).arg(want));
    };
    check(bg == p.color(QPalette::Base), QStringLiteral("%1: the background is the palette's base").arg(QLatin1String(name)));
    at(S::Foreground, 7.0);
    for (int r = S::Color1; r <= S::Color6; ++r)
        at(r, 4.5);
    for (int r = S::Color1Intense; r <= S::Color6Intense; ++r)
        at(r, 3.0);
    const bool dark = QColor(p.color(QPalette::Base)).lightnessF() < QColor(p.color(QPalette::Text)).lightnessF();
    at(dark ? S::Color0Intense : S::Color7, 3.0);   // the grey
    at(dark ? S::Color7 : S::Color0, 7.0);          // the text-like one
    at(dark ? S::Color0 : S::Color7Intense, 1.4);   // a shade off the background
    // The six colours told apart: no two alike.
    for (int a = S::Color1; a <= S::Color6; ++a)
        for (int b = a + 1; b <= S::Color6; ++b) {
            const QColor x = s.colors[a], y = s.colors[b];
            const int d = qAbs(x.red() - y.red()) + qAbs(x.green() - y.green()) + qAbs(x.blue() - y.blue());
            check(d > 60, QStringLiteral("%1: %2 %3 and %4 %5 look alike").arg(QLatin1String(name), S::roleLabel(a),
                                                                               x.name(), S::roleLabel(b), y.name()));
        }
}

}

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    readable("Breeze dark", palette("#141618", "#fcfcfc", "#3daee9"));
    readable("Breeze light", palette("#ffffff", "#232629", "#3daee9"));
    readable("pure black", palette("#000000", "#ffffff", "#ff0000"));
    readable("Darcula", palette("#2b2b2b", "#a9b7c6", "#4a88c7"));
    readable("Solarized light", palette("#fdf6e3", "#657b83", "#268bd2"));
    readable("low-contrast text", palette("#2b2b2b", "#555555", "#00aa66"));

    // The nearest colour leans towards the accent: a purple accent's blue
    // is more purple (more red in it) than a cyan accent's.
    {
        const QColor towardPurple = S::systemScheme(palette("#141618", "#fcfcfc", "#7a5cff")).colors[S::Color4];
        const QColor towardCyan = S::systemScheme(palette("#141618", "#fcfcfc", "#3daee9")).colors[S::Color4];
        check(towardPurple.red() > towardCyan.red(),
              QStringLiteral("the blue leans to the accent: %1 (purple), %2 (cyan)").arg(towardPurple.name(), towardCyan.name()));
    }

    // The file: the same for the same colours, new for new ones; and the
    // scheme lists offer System first.
    QApplication::setPalette(palette("#141618", "#fcfcfc", "#3daee9"));
    const QString a = S::systemSchemeFile();
    check(!a.isEmpty() && a == S::systemSchemeFile(), QStringLiteral("the same file for the same colours: %1").arg(a));
    check(S::findSchemeFile(S::systemName()) == a, QStringLiteral("findSchemeFile(System) is it"));
    S loaded;
    bool same = loaded.load(a);
    const S made = S::systemScheme(QApplication::palette());
    for (int r = 0; r < S::RoleCount; ++r)
        same = same && loaded.colors[r].name() == made.colors[r].name();
    check(same, QStringLiteral("the file has the colours"));
    QApplication::setPalette(palette("#ffffff", "#232629", "#3daee9"));
    const QString b = S::systemSchemeFile();
    check(!b.isEmpty() && b != a, QStringLiteral("a new file for new colours: %1").arg(b));
    check(!QFile::exists(a), QStringLiteral("the old file goes"));
    check(S::allSchemeNames().value(0) == S::systemName(), QStringLiteral("System first in the list"));
    QFile::remove(b);

    if (failures == 0)
        QTextStream(stdout) << "test-systemscheme: all passed\n";
    return failures ? 1 : 0;
}
