/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QDialog>
#include <QList>

class Settings;
class ProfileStore;
class QListWidget;
class QStackedWidget;
class QComboBox;
class QFormLayout;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QGroupBox;
class QVBoxLayout;

/*
 * Sectioned modal settings dialog.
 *
 * Every page is built the same way: titled groups, each a form whose label
 * column is the same width on every page, so fields don't shift sideways when
 * switching sections. Checkboxes sit in the field column like any other
 * control, and hints sit under the control they explain.
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

    enum Section { Appearance = 0, Terminal, Window, Profiles };

    /* Open on a specific page. */
    void showSection(Section section);

private:
    QWidget *buildAppearancePage();
    QWidget *buildTerminalPage();
    QWidget *buildWindowPage();
    QWidget *buildProfilesPage();
    void syncFromSettings();

    /*
     * Add a titled group to a page and return its form. Forms made here get
     * their label column aligned across all pages once every page is built.
     */
    QFormLayout *addGroup(QVBoxLayout *page, const QString &title);
    void alignFormLabels();

    /* Profiles page plumbing. */
    void rebuildProfileList(const QString &selectId = QString());
    void loadProfileIntoForm(const QString &id);
    void commitProfileForm();
    QString currentProfileId() const;
    void setProfileFormEnabled(bool on);

    /*
     * Open the colour scheme editor seeded from the current scheme, then make
     * the saved scheme selectable and active.
     */
    void editColorScheme();

    /*
     * Rebuild the scheme combo from qtermwidget's list.
     *
     * Needed because availableColorSchemes() caches on first call: a scheme
     * saved during this run is NOT returned by it, so the combo has to be told
     * about new names explicitly.
     */
    void reloadSchemeBox(const QString &selectName = QString());

    /*
     * Repopulate the profile form's colour-scheme combo, keeping the current
     * selection.
     *
     * Separate from reloadSchemeBox() because that one drives the global
     * setting; this one must not, or refreshing the list would apply a scheme
     * as a side effect.
     */
    void reloadProfileSchemeBox(const QString &selectName = QString());

    Settings &m_settings;
    ProfileStore &m_profiles;
    QListWidget *m_sections = nullptr;
    QLabel *m_pageTitle = nullptr;
    QStackedWidget *m_pages = nullptr;
    QList<QFormLayout *> m_forms;

    // Appearance.
    QComboBox *m_schemeBox = nullptr;
    QToolButton *m_editSchemeBtn = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityLabel = nullptr;
    QComboBox *m_fontBox = nullptr;
    QDoubleSpinBox *m_fontSizeBox = nullptr;
    QSpinBox *m_lineSpacingBox = nullptr;
    QSpinBox *m_marginBox = nullptr;
    QCheckBox *m_boldBox = nullptr;
    QCheckBox *m_italicBox = nullptr;
    QCheckBox *m_antialiasBox = nullptr;
    QCheckBox *m_lineCharsBox = nullptr;
    QCheckBox *m_bidiBox = nullptr;

    // Terminal.
    QComboBox *m_cursorShapeBox = nullptr;
    QCheckBox *m_blinkBox = nullptr;
    QSpinBox *m_scrollbackBox = nullptr;
    QComboBox *m_scrollbarPosBox = nullptr;
    QCheckBox *m_pasteConfirmBox = nullptr;
    QCheckBox *m_pasteTrimBox = nullptr;
    QCheckBox *m_flowControlWarnBox = nullptr;

    // Tabs & Window.
    QComboBox *m_tabLayoutBox = nullptr;
    QCheckBox *m_monitorActivityBox = nullptr;
    QCheckBox *m_monitorSilenceBox = nullptr;
    QSpinBox *m_silenceTimeoutBox = nullptr;
    QCheckBox *m_confirmCloseBox = nullptr;

    // Profiles page.
    QListWidget *m_profileList = nullptr;
    QLineEdit *m_profileName = nullptr;
    QCheckBox *m_profileDefaultBox = nullptr;
    QComboBox *m_profileScheme = nullptr;
    QComboBox *m_profileFont = nullptr;
    QDoubleSpinBox *m_profileFontSize = nullptr;
    QLineEdit *m_profileShell = nullptr;
    QLineEdit *m_profileShellArgs = nullptr;
    QLineEdit *m_profileWorkDir = nullptr;
    QToolButton *m_profileAdd = nullptr;
    QToolButton *m_profileDuplicate = nullptr;
    QToolButton *m_profileDelete = nullptr;
    QGroupBox *m_profileFormBox = nullptr;
    /*
     * Suppresses the "commit on edit" handlers while the form is being filled
     * programmatically; without it, loading a profile would write it straight
     * back and could stomp a field mid-load.
     */
    bool m_loadingProfile = false;
};
