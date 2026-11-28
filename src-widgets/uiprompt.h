/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QString>
#include <QList>

#include <functional>
#include <QStringList>

class QDialog;
class QWidget;

/*
 * The app's own small dialogs, drawn as a Rex server's prompts are
 * (rex.ui.input, rex.ui.pick; popupframe.h): a frameless rounded panel with
 * a soft shadow, over the window. Used instead of QInputDialog and
 * QMessageBox, so asking for a name looks like asking from a Rex server.
 * Files and folders are still chosen with the system's own dialogs.
 */
namespace uiprompt {

/* A line of text: `placeholder` in the empty field, `value` to start with,
 * `confirm` naming what Return does ("Open", "Rename"). Empty and *ok false
 * when cancelled. */
QString text(QWidget *parent, const QString &title, const QString &placeholder,
             const QString &value = QString(), bool *ok = nullptr, const QString &confirm = QString());

/* One of `items` (filtered as you type), or with `allowOther` what was
 * typed. Empty and *ok false when cancelled. */
QString pick(QWidget *parent, const QString &title, const QStringList &items,
             bool *ok = nullptr, bool allowOther = false, const QString &placeholder = QString());

/* A question: true for `action` (a destructive one in red), false for
 * Cancel or Esc. */
bool confirm(QWidget *parent, const QString &title, const QString &text, const QString &action,
             bool destructive = false);

/* Something that went wrong, or to know: an OK to close it. `detail` (a
 * command's output, say) is shown below in a small scrolling box. */
void message(QWidget *parent, const QString &title, const QString &text, const QString &detail = QString());

/* A choice of several answers (and Cancel): the index of the one chosen,
 * or -1. Return gives `defaultChoice` (-1: Cancel). */
struct Choice
{
    QString label;
    bool destructive = false;
    QString name;   // the button's object name, for tests
};
int choose(QWidget *parent, const QString &title, const QString &text, const QList<Choice> &choices,
           int defaultChoice = 0, const QString &detail = QString());

/* A dialog of the app's own (a small form) in the same panel: frameless,
 * its title as a heading at the top, over the window as the prompts are.
 * Call once its layout is set, before showing it. */
void frame(QDialog *dialog);

/* choose() without waiting: `done` gets the answer when it's given. The
 * dialog, which deletes itself after, is returned (to reject it if what it
 * asks about goes away first). */
QDialog *chooseLater(QWidget *parent, const QString &title, const QString &text, const QList<Choice> &choices,
                     int defaultChoice, const QString &detail, std::function<void(int)> done);

} // namespace uiprompt
