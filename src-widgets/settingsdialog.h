/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>

class Settings;
class ProfileStore;
class QListWidget;
class QStackedWidget;
class QComboBox;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QGroupBox;

/*
 * Sectioned modal settings dialog, mirroring the QML build's pages plus a
 * Profiles page the QML build never had.
 *
 * Edits are applied live through Settings / ProfileStore (which emit and
 * persist), so the dialog itself holds no state that can drift from what the
 * terminals show. The Profiles page is the exception: a half-typed profile
 * name should not be written to disk on every keystroke, so it edits a working
 * copy and commits on field change / selection change.
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(Settings &settings, ProfileStore &profiles,
                   QWidget *parent = nullptr);

    enum Section { Appearance = 0, Text, Cursor, Scrolling, Profiles };

    /* Open on a specific page, e.g. from the sidebar's "settings" affordance. */
    void showSection(Section section);

private:
    QWidget *buildAppearancePage();
    QWidget *buildTextPage();
    QWidget *buildCursorPage();
    QWidget *buildScrollingPage();
    QWidget *buildProfilesPage();
    void syncFromSettings();

    /* Profiles page plumbing. */
    void rebuildProfileList(const QString &selectId = QString());
    void loadProfileIntoForm(const QString &id);
    void commitProfileForm();
    QString currentProfileId() const;
    void setProfileFormEnabled(bool on);

    Settings &m_settings;
    ProfileStore &m_profiles;
    QListWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;

    QComboBox *m_schemeBox = nullptr;
    QSlider *m_fontSizeSlider = nullptr;
    QLabel *m_fontSizeLabel = nullptr;
    QComboBox *m_fontBox = nullptr;
    QCheckBox *m_boldBox = nullptr;
    QCheckBox *m_italicBox = nullptr;
    QCheckBox *m_antialiasBox = nullptr;
    QCheckBox *m_blinkBox = nullptr;
    QComboBox *m_cursorShapeBox = nullptr;
    QCheckBox *m_lineCharsBox = nullptr;
    QComboBox *m_tabLayoutBox = nullptr;
    QSpinBox *m_marginBox = nullptr;
    QCheckBox *m_bidiBox = nullptr;
    QComboBox *m_scrollbarPosBox = nullptr;
    QCheckBox *m_flowControlWarnBox = nullptr;
    QCheckBox *m_confirmCloseBox = nullptr;
    QCheckBox *m_monitorActivityBox = nullptr;
    QCheckBox *m_monitorSilenceBox = nullptr;
    QSpinBox *m_silenceTimeoutBox = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityLabel = nullptr;
    QSpinBox *m_scrollbackBox = nullptr;

    // Profiles page.
    QListWidget *m_profileList = nullptr;
    QLineEdit *m_profileName = nullptr;
    QComboBox *m_profileScheme = nullptr;
    QComboBox *m_profileFont = nullptr;
    QDoubleSpinBox *m_profileFontSize = nullptr;
    QLineEdit *m_profileShell = nullptr;
    QLineEdit *m_profileShellArgs = nullptr;
    QLineEdit *m_profileWorkDir = nullptr;
    QPushButton *m_profileAdd = nullptr;
    QPushButton *m_profileDuplicate = nullptr;
    QPushButton *m_profileDelete = nullptr;
    QGroupBox *m_profileFormBox = nullptr;
    /*
     * Suppresses the "commit on edit" handlers while the form is being filled
     * programmatically; without it, loading a profile would write it straight
     * back and could stomp a field mid-load.
     */
    bool m_loadingProfile = false;
};
