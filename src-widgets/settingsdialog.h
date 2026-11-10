/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>

class Settings;
class QListWidget;
class QStackedWidget;
class QComboBox;
class QSlider;
class QSpinBox;
class QCheckBox;
class QLabel;

/*
 * Sectioned modal settings dialog, mirroring the QML build's four pages.
 *
 * Edits are applied live through Settings (which emits and persists), so the
 * dialog itself holds no state that can drift from what the terminals show.
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(Settings &settings, QWidget *parent = nullptr);

    enum Section { Appearance = 0, Text, Cursor, Scrolling };

    /* Open on a specific page, e.g. from the sidebar's "settings" affordance. */
    void showSection(Section section);

private:
    QWidget *buildAppearancePage();
    QWidget *buildTextPage();
    QWidget *buildCursorPage();
    QWidget *buildScrollingPage();
    void syncFromSettings();

    Settings &m_settings;
    QListWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;

    QComboBox *m_schemeBox = nullptr;
    QSlider *m_fontSizeSlider = nullptr;
    QLabel *m_fontSizeLabel = nullptr;
    QComboBox *m_fontBox = nullptr;
    QSpinBox *m_lineSpacingBox = nullptr;
    QCheckBox *m_boldBox = nullptr;
    QCheckBox *m_italicBox = nullptr;
    QCheckBox *m_antialiasBox = nullptr;
    QCheckBox *m_blinkBox = nullptr;
    QComboBox *m_tabLayoutBox = nullptr;
    QSpinBox *m_scrollbackBox = nullptr;
};
