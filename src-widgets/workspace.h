/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QColor>
#include <QDialog>
#include <QIcon>
#include <QList>
#include <QPointer>
#include <QString>

class QButtonGroup;
class QLineEdit;
class TerminalTab;

/*
 * A workspace: a named group of tabs, such as one per project. The window
 * shows one workspace at a time; the others' tabs are kept, programs still
 * running, until it is shown again.
 */
struct Workspace
{
    QString id;       // stable, for menus and saved state
    QString name;
    QString root;     // where new tabs start; empty for the profile's default
    QColor colour;    // optional accent, to tell workspaces apart

    // While another workspace is shown: its tabs, in order, and which was
    // current. (The shown workspace's tabs are the tab widget's.)
    QList<QPointer<TerminalTab>> parked;
    int currentTab = 0;
    // Output arrived in one of its tabs while another workspace was shown.
    bool unseenOutput = false;

    /* A coloured dot for menus and buttons (a hollow one without a colour). */
    QIcon icon(const QColor &outline) const;
};

/* Edit a workspace's name, folder and colour. */
class WorkspaceDialog : public QDialog
{
    Q_OBJECT

public:
    /* Edit `ws` in place; false if cancelled. */
    static bool edit(QWidget *parent, const QString &title, Workspace &ws);

private:
    explicit WorkspaceDialog(const QString &title, const Workspace &ws, QWidget *parent);

    QLineEdit *m_name = nullptr;
    QLineEdit *m_root = nullptr;
    QButtonGroup *m_colours = nullptr;
};
