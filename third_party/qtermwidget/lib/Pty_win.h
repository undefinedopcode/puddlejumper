/*
    puddlejumper: the Windows counterpart of Pty.h, on the pseudoconsole API
    (ConPTY, Windows 10 1809 and later). Pty.h includes this in its place.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#ifndef PTY_WIN_H
#define PTY_WIN_H

#include <QObject>
#include <QProcess>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

class QWinEventNotifier;

namespace Konsole {

struct ConPtyState;

/**
 * Starts the terminal process in a pseudoconsole, and passes data between
 * it and the emulation: the interface Session uses of the Unix Pty, which
 * is a QProcess there; the members of QProcess that Session uses are here
 * under the same names.
 *
 * The pseudoconsole is made by start(), so a Pty that never starts a
 * process (a terminal fed from elsewhere) costs nothing.
 */
class Pty : public QObject
{
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    /**
     * Starts the terminal process: 0 if it started, negative otherwise.
     * As on Unix, the first of @p arguments is the program's own name.
     */
    int start(const QString &program, const QStringList &arguments,
              const QStringList &environment, ulong winid, bool addToUtmp);

    /** Nothing to set: these are properties of a tty. */
    void setEmptyPTYProperties() {}
    void setWriteable(bool) {}

    /** Kept, not applied: a pseudoconsole has no Xon/Xoff or erase setting. */
    void setFlowControlEnabled(bool on) { _xonXoff = on; }
    bool flowControlEnabled() const { return _xonXoff; }
    void setErase(char erase) { _eraseChar = erase; }
    char erase() const { return _eraseChar; }

    void setWindowSize(int lines, int cols);
    // puddlejumper: ConPTY has no size in pixels.
    void setCellPixelSize(const QSize &) {}
    QSize windowSize() const { return {_windowColumns, _windowLines}; }

    void setInitialWorkingDirectory(const QString &dir) { _workingDirectory = dir; }

    /**
     * Windows has no foreground process group. The nearest thing: the newest
     * process started (at any depth) under the shell, or the shell itself at
     * its prompt. 0 without a process.
     */
    int foregroundProcessGroup() const;

    /**
     * Closes the pseudoconsole, as a terminal window closing does: the
     * programs in it are told to exit, and ended if they don't. finished()
     * follows.
     */
    void closePty();

    /* As QProcess's. */
    QProcess::ProcessState state() const;
    qint64 processId() const { return _processId; }
    bool waitForFinished(int msecs = 30000);
    QProcess::ExitStatus exitStatus() const { return QProcess::NormalExit; }
    void kill();

public slots:
    void setUtf8Mode(bool) {}
    void lockPty(bool) {}
    void sendData(const char *buffer, int length);

signals:
    void receivedData(const char *buffer, int length);
    void finished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    /* Passes on what the reading thread has collected; finishes at its end. */
    void drain();
    void processExited();

    std::shared_ptr<ConPtyState> _state;   // shared with the threads
    QWinEventNotifier *_exitNotifier = nullptr;
    QString _workingDirectory;
    qint64 _processId = 0;
    int _exitCode = 0;
    bool _exited = false;      // the process has
    bool _finished = false;    // finished() was emitted
    int _windowColumns = 80;
    int _windowLines = 24;
    char _eraseChar = 0;
    bool _xonXoff = true;
};

}

#endif // PTY_WIN_H
