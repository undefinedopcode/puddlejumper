/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>
#include <QVector>
#include <QWidget>

#include "colorscheme.h"

class QLineEdit;
class QPushButton;

/*
 * A miniature terminal drawn from a scheme, used as the editor's preview.
 *
 * Deliberately not a QTermWidget: applying a scheme to a real terminal needs a
 * file on disk (the scheme manager caches by basename and has no in-memory
 * table setter), so a preview would be slow and would litter temp files.
 * Painting the sample directly is exact, instant and side-effect free.
 */
class SchemePreview : public QWidget
{
    Q_OBJECT

public:
    explicit SchemePreview(QWidget *parent = nullptr);
    void setScheme(const ColorSchemeFile &s);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    ColorSchemeFile m_scheme;
};

/*
 * Colour scheme editor.
 *
 * Always edits a COPY. The dialog is seeded from an existing scheme (or the
 * built-in default) and saves under a new name, so a system preset can never be
 * overwritten and there is no need to ask which schemes are safe to touch.
 *
 * All 20 Konsole roles are editable. The ANSI palette is shown as a grid of
 * swatches with the text/background pair above it, because those two are what
 * a scheme is usually judged on.
 */
class ColorSchemeEditor : public QDialog
{
    Q_OBJECT

public:
    /*
     * `seedName` may be any scheme qtermwidget knows; the file is located and
     * loaded so the copy starts from its colours. If it cannot be found, the
     * built-in default palette is used.
     *
     * If the seed is one of the user's own schemes it is edited IN PLACE, under
     * its existing name. Built-in schemes are edited as a renamed copy, so a
     * preset is never overwritten.
     */
    explicit ColorSchemeEditor(const QString &seedName, QWidget *parent = nullptr);

    /* The scheme as edited. Valid after the dialog is accepted. */
    const ColorSchemeFile &scheme() const { return m_scheme; }

    /* Where it was saved (empty if not saved). */
    QString savedPath() const { return m_savedPath; }

    /* Base name it was saved as — use this to refresh lists. */
    QString savedName() const { return m_savedName; }

    /*
     * True when an existing scheme of the user's own was overwritten, false
     * when this created a new scheme (the copy case).
     *
     * The caller needs this because qtermwidget caches schemes by basename and
     * cannot reload one in place; an overwrite has to be applied through a
     * throwaway copy file or the terminal keeps showing the old colours.
     */
    bool overwroteExisting() const { return m_overwroteExisting; }

private:
    QWidget *buildPreview();
    void refreshSwatches();
    void refreshPreview();
    void pickColor(int role);
    void save();

    ColorSchemeFile m_scheme;
    QString m_savedPath;
    QString m_savedName;
    // True when the seed was the user's own scheme, so its name is reused.
    bool m_editingOwnScheme = false;
    bool m_overwroteExisting = false;
    QString m_originalName;

    QLineEdit *m_nameEdit = nullptr;
    QVector<QPushButton *> m_swatches;   // indexed by ColorSchemeFile::Role
    SchemePreview *m_preview = nullptr;
};
