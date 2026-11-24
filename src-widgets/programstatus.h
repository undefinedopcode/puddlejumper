/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <optional>

/*
 * The Program Status Protocol (OSC 7501): a program in a terminal says what it
 * is doing (working, waiting on the user, done, failed) and why, and the
 * terminal keeps it as records, one per id, and shows them how it likes.
 * Spec: https://www.superlogical.com/rex/docs/build/program-status (rev. 0.4).
 *
 *   OSC 7501 ; state=blocked:kind=permission:app=terraform:msg=<base64> ST
 *
 * The emulator hands over each report's body (after "7501;") and answers the
 * support query itself; this is the rest: parsing to the spec's limits, the
 * records, and when they go.
 */
struct ProgramStatusRecord
{
    enum State { None, Idle, Working, Done, Blocked, Error };

    State state = None;
    QString id;         // empty: the root record
    QString kind;       // blocked only: permission, question or auth
    int progress = -1;  // working and blocked only: 0 to 100, -1 indeterminate
    QString app;        // as reported; see ProgramStatus::app() for inheritance
    QString title;      // decoded, single line
    QString msg;        // decoded, single line
    quint64 updated = 0;
    bool seen = false;  // done or error the user has been back to

    bool finished() const { return state == Done || state == Error; }
};

class ProgramStatus : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    struct Report {
        ProgramStatusRecord record;
        bool clear = false;   // state=clear: remove record.id and below
    };
    /* One report's body, checked against the spec; nothing if it is to be
     * ignored or discarded. */
    static std::optional<Report> parse(const QString &body);

    /* Apply a report's body. False if it was ignored or discarded. */
    bool apply(const QString &body);
    /* A full reset of the terminal (RIS): every record goes. */
    void reset();
    /* The program exited, or the shell showed a prompt: working, blocked and
     * idle records go; done and error stay until seen. */
    void programEnded();
    /* The user is back at this terminal (a key pressed in it): its done and
     * error records have been seen. */
    void markSeen();

    QList<ProgramStatusRecord> records() const { return m_records; }
    bool isEmpty() const { return m_records.isEmpty(); }
    /* A record's app, or its nearest ancestor's. */
    QString app(const ProgramStatusRecord &record) const;
    /* The record most worth showing: blocked, then unseen error, unseen done,
     * working; the latest of a kind. None if there is nothing to show. */
    ProgramStatusRecord mostPressing() const;

    /* How much a record wants showing: blocked 4, unseen error 3, unseen
     * done 2, working 1; 0 for nothing to show. */
    static int rank(const ProgramStatusRecord &record);
    /* A short label for a record's state ("Needs approval", "Working 40%"). */
    static QString label(const ProgramStatusRecord &record);

signals:
    void changed();

private:
    QList<ProgramStatusRecord> m_records;
    quint64 m_clock = 0;
};
