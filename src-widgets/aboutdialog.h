/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>

/*
 * Help > About PuddleJumper: the logo, name and version (with the commit
 * it was built from), what it runs on, the licence and where it lives. In
 * the popups' frame (popupframe.h), centred over the window. Esc, Return or
 * Close dismisses it; Copy Version Info puts the version lines on the
 * clipboard, for a bug report.
 */
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    /* The version lines, as copied: name and version, build, Qt, system. */
    static QString versionInfo();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
};
