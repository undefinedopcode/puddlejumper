/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

/*
 * The Program Resume Protocol (OSC 7577, Superlogical's version 0.5), for a
 * local tab: a program tells the terminal how to start it again in its
 * current state,
 *
 *   ESC ] 7577 ; pid=P:time=T:argv=B64,B64,...[:cwd=B64]:mac=HEX ST
 *   ESC ] 7577 ; pid=P:time=T:clear=1:mac=HEX ST
 *
 * mac being SHA-256 of the body before ":mac=", ":" and the token the tab
 * put in OSC7577_TOKEN for its shell. The emulator hands over each report's
 * body (after "7577;"). Records are per owner pid, and one whose owner has
 * exited, or left the tab's session, is gone. When the window saves its
 * session, a tab's newest record goes with it if a trust rule
 * (resumerules.h) trusts it, and on restore the command is run again.
 *
 * Reports with a bad mac or time are ignored; authenticated reports that are
 * malformed, break a limit or need a key this doesn't know are rejected,
 * which removes their owner's record.
 */
class ProgramResume
{
public:
    struct Record
    {
        int pid = 0;
        QStringList argv;
        QString cwd;         // as reported, if it was
        qint64 time = 0;
        quint64 order = 0;
    };

    enum class Verdict { Ignore, Reject, Set, Clear };
    struct Report
    {
        int pid = 0;
        qint64 time = 0;
        QStringList argv;
        QString cwd;
    };
    /* A report's body checked against the token at `now` (seconds). */
    static Verdict parse(const QString &body, const QString &token, qint64 now, Report *out);

    /* A new token: 32 random characters of [A-Za-z0-9_-]. */
    static QString newToken();

    explicit ProgramResume(const QString &token = newToken()) : m_token(token) {}

    QString token() const { return m_token; }

    /* Takes a report from the tab whose shell is `leader`: true if the
     * records changed. */
    bool apply(const QString &body, int leader);

    /* The live records, newest first (owners that have gone are dropped). */
    QList<Record> records(int leader);

    /* Whether `pid` is alive and in the session `leader` leads. */
    static bool owns(int pid, int leader);

    /*
     * "puddlejumper --resume-helper FILE", which a restored local tab's
     * shell runs to start a program again (TerminalWidget::resumeProgram):
     * FILE holds {argv, cwd}. It is read and removed, then the program
     * replaces this process, so it runs as the shell's foreground job,
     * found on the shell's PATH, its arguments as they were (never shell
     * source). Called before any QApplication; returns only on failure.
     */
    static int runHelper(const char *file);

private:
    QString m_token;
    QHash<int, Record> m_records;
    QHash<int, qint64> m_lastTime;   // the newest time taken per pid
    quint64 m_order = 0;
};
