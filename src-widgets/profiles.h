/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

/*
 * A terminal profile: a named bundle of per-terminal overrides.
 *
 * Every field is optional. std::nullopt means "inherit the global setting",
 * which is deliberately not the same as an empty string: a profile may want to
 * force the *empty* shell (the login shell) while a global override exists.
 * Using optional makes that distinction expressible and checkable, where a
 * sentinel string would make it a trap.
 */
struct TerminalProfile
{
    /*
     * Stable identity, independent of the display name. Tabs keep pointing at
     * a profile by id, so renaming a profile does not orphan open tabs, and
     * duplicate names cannot collide. Assigned by ProfileStore.
     */
    QString id;

    QString name;

    /*
     * Theme: colour scheme name. Stored as a bare name (the file's base name);
     * TerminalWidget resolves it to a file on disk, which is what lets a
     * user-saved scheme work here too.
     */
    std::optional<QString> colorScheme;

    /* Text: font family and size in points. Unset inherits the global font. */
    std::optional<QString> fontFamily;
    std::optional<double>  fontSize;

    /*
     * Shell: program plus arguments. Left unset, the login shell is used.
     * Note that an empty argument list is meaningful, so args is only
     * consulted when shellProgram is set.
     */
    std::optional<QString>     shellProgram;
    std::optional<QStringList> shellArgs;

    /* Start directory. Unset starts in the default (the user's home). */
    std::optional<QString> workingDirectory;

    bool isDefault = false;
};

/*
 * Owns the profile collection and persists it via QSettings.
 *
 * The store guarantees two invariants that the rest of the app relies on:
 *
 *  1. There is always at least one profile, and exactly one has isDefault set.
 *     The default profile is what "New Tab" uses, so opening a tab can never
 *     fail for want of a profile.
 *  2. The default profile may be edited and renamed but not deleted. That
 *     keeps "the thing New Tab opens" always well defined.
 *
 * Config layout (keys under [profiles]):
 *
 *   profiles/order        = id1,id2,...   (display order)
 *   profiles/default      = id
 *   profiles/<id>/name    = ...
 *   profiles/<id>/colorScheme, fontFamily, fontSize,
 *              shellProgram, shellArgs, workingDirectory
 *
 * Only set overrides are written, so an inheriting profile stays small and a
 * later global change still reaches it.
 */
class ProfileStore : public QObject
{
    Q_OBJECT

public:
    explicit ProfileStore(QObject *parent = nullptr);

    /* In display order. Never empty. */
    const QVector<TerminalProfile> &profiles() const { return m_profiles; }

    /* The profile "New Tab" uses. Never null. */
    const TerminalProfile &defaultProfile() const;

    /* Look up by id; a dangling id falls back to the default profile. */
    const TerminalProfile &profileById(const QString &id) const;

    /* Appends a profile and returns its new (generated) id. */
    QString add(const TerminalProfile &profile);

    /* Copy an existing profile under "Name copy". Returns the new id, or a
     * null string if sourceId is unknown. */
    QString duplicate(const QString &sourceId);

    /*
     * Replace the stored profile with the same id. Returns false if the id is
     * unknown. The default profile's isDefault flag cannot be changed here.
     */
    bool update(const TerminalProfile &profile);

    /*
     * Remove a profile. Refuses the default profile and unknown ids; returns
     * false in both cases. Tabs already open under that profile keep their
     * resolved settings (they are not retroactively changed).
     */
    bool remove(const QString &id);

    /* Make an existing profile the one "New Tab" uses. False if unknown. */
    bool setDefault(const QString &id);

signals:
    /* Any change to the collection or to a profile's contents. */
    void profilesChanged();

private:
    void load();
    void save();

    QVector<TerminalProfile> m_profiles;
    QString m_defaultId;
};
