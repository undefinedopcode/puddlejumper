/*
    puddlejumper: Pty for Windows, on the pseudoconsole API. See Pty_win.h.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "Pty_win.h"

#include <QCoreApplication>
#include <QDir>
#include <QMutex>
#include <QPointer>
#include <QProcessEnvironment>
#include <QWaitCondition>
#include <QWinEventNotifier>
#include <QtDebug>

#include <atomic>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

using namespace Konsole;

namespace Konsole {

/*
 * What the Pty shares with its threads, which may outlive it: the one
 * reading the pseudoconsole's output, and the one closing it (closing can
 * wait on the programs inside, for seconds).
 */
struct ConPtyState
{
    HPCON console = nullptr;
    HANDLE input = INVALID_HANDLE_VALUE;    // our end: written to
    HANDLE output = INVALID_HANDLE_VALUE;   // our end: read, by the thread
    HANDLE process = nullptr;
    std::atomic<bool> closing{false};

    QMutex mutex;
    QWaitCondition notFull;
    QByteArray buffer;        // read, not yet passed on
    bool wakePosted = false;
    bool eof = false;
    bool abandoned = false;   // the Pty is gone: discard

    ~ConPtyState()
    {
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE)
            CloseHandle(output);
        if (process)
            CloseHandle(process);
    }
};

}

namespace {

// Output held for the emulation before the reading thread waits for it.
constexpr qsizetype kBufferLimit = 1 << 20;

/* One argument, quoted as the C runtime's command line parsing undoes it. */
QString quoteArgument(const QString &arg)
{
    if (!arg.isEmpty() && !arg.contains(QLatin1Char(' ')) && !arg.contains(QLatin1Char('\t'))
        && !arg.contains(QLatin1Char('"')))
        return arg;
    QString out = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
        } else if (c == QLatin1Char('"')) {
            out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            backslashes = 0;
            out += c;
        } else {
            out += QString(backslashes, QLatin1Char('\\'));
            backslashes = 0;
            out += c;
        }
    }
    out += QString(backslashes * 2, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}

/* Closes the pseudoconsole, once, away from the calling thread. */
void closeConsole(const std::shared_ptr<ConPtyState> &state)
{
    if (!state->console || state->closing.exchange(true))
        return;
    std::thread([state]() { ClosePseudoConsole(state->console); }).detach();
}

}

Pty::Pty(QObject *parent)
    : QObject(parent)
{
}

Pty::~Pty()
{
    if (!_state)
        return;
    delete _exitNotifier;
    {
        QMutexLocker lock(&_state->mutex);
        _state->abandoned = true;
        _state->buffer.clear();
        _state->notFull.wakeAll();
    }
    // The shell at once; whatever else is in the console as it closes.
    if (!_exited)
        TerminateProcess(_state->process, 1);
    closeConsole(_state);
}

int Pty::start(const QString &program, const QStringList &arguments,
               const QStringList &environment, ulong winid, bool addToUtmp)
{
    Q_UNUSED(winid)
    Q_UNUSED(addToUtmp)
    if (_state)
        return -1;

    auto state = std::make_shared<ConPtyState>();

    // Two pipes: the console reads its input from one and writes its output
    // to the other; it keeps its ends itself, so ours of those are closed.
    HANDLE consoleInput = INVALID_HANDLE_VALUE, consoleOutput = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&consoleInput, &state->input, nullptr, 0)
        || !CreatePipe(&state->output, &consoleOutput, nullptr, 0)) {
        qWarning() << "Pty: no pipes for the pseudoconsole:" << qt_error_string(GetLastError());
        if (consoleInput != INVALID_HANDLE_VALUE)
            CloseHandle(consoleInput);
        return -1;
    }
    const COORD size{SHORT(qMax(1, _windowColumns)), SHORT(qMax(1, _windowLines))};
    const HRESULT made = CreatePseudoConsole(size, consoleInput, consoleOutput, 0, &state->console);
    CloseHandle(consoleInput);
    CloseHandle(consoleOutput);
    if (FAILED(made)) {
        qWarning() << "Pty: no pseudoconsole:" << qt_error_string(made);
        state->console = nullptr;
        return -1;
    }

    // The process is attached to the console by an attribute of its startup.
    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    QByteArray attributes(qsizetype(attributesSize), '\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Without this the process would get our own standard handles where we
    // have them (started from a console, or redirected), not the console's.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributesSize)
        || !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                      state->console, sizeof(HPCON), nullptr, nullptr)) {
        qWarning() << "Pty: can't attach to the pseudoconsole:" << qt_error_string(GetLastError());
        ClosePseudoConsole(state->console);
        return -1;
    }

    QStringList words{quoteArgument(QDir::toNativeSeparators(program))};
    for (const QString &arg : arguments.mid(1))
        words << quoteArgument(arg);
    QString commandLine = words.join(QLatin1Char(' '));

    // Our environment, with what was asked for; the block is name=value
    // strings in name order, each ended by a NUL, and one more at the end.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &pair : environment) {
        const int pos = pair.indexOf(QLatin1Char('='));
        if (pos > 0)
            env.insert(pair.left(pos), pair.mid(pos + 1));
    }
    if (!env.contains(QStringLiteral("TERM")))
        env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    QStringList pairs = env.toStringList();
    pairs.sort(Qt::CaseInsensitive);
    QString block;
    for (const QString &pair : std::as_const(pairs)) {
        block += pair;
        block += QChar(u'\0');
    }
    block += QChar(u'\0');

    const QString directory = QDir::toNativeSeparators(_workingDirectory);
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(
        nullptr, reinterpret_cast<LPWSTR>(commandLine.data()), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        const_cast<void *>(static_cast<const void *>(block.utf16())),
        directory.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(directory.utf16()),
        &startup.StartupInfo, &info);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) {
        qWarning().noquote() << "Pty: can't start" << commandLine << "-" << qt_error_string(error);
        ClosePseudoConsole(state->console);
        return -1;
    }
    CloseHandle(info.hThread);
    state->process = info.hProcess;
    _processId = info.dwProcessId;
    _state = state;

    _exitNotifier = new QWinEventNotifier(state->process, this);
    connect(_exitNotifier, &QWinEventNotifier::activated, this, &Pty::processExited);

    // The reading thread. It ends when the console is closed, and tells this
    // object (if it is still there) when there is something to pass on.
    std::thread([state, pty = QPointer<Pty>(this)]() {
        auto wake = [&pty]() {
            QMetaObject::invokeMethod(qApp, [pty]() {
                if (pty)
                    pty->drain();
            }, Qt::QueuedConnection);
        };
        char chunk[16384];
        DWORD count = 0;
        while (ReadFile(state->output, chunk, sizeof(chunk), &count, nullptr) && count > 0) {
            QMutexLocker lock(&state->mutex);
            while (!state->abandoned && state->buffer.size() > kBufferLimit)
                state->notFull.wait(&state->mutex);
            if (state->abandoned)
                continue;
            state->buffer.append(chunk, count);
            if (!state->wakePosted) {
                state->wakePosted = true;
                wake();
            }
        }
        QMutexLocker lock(&state->mutex);
        state->eof = true;
        if (!state->abandoned)
            wake();
    }).detach();

    return 0;
}

void Pty::drain()
{
    if (!_state)
        return;
    QByteArray data;
    bool eof;
    {
        QMutexLocker lock(&_state->mutex);
        data.swap(_state->buffer);
        _state->wakePosted = false;
        eof = _state->eof;
        _state->notFull.wakeAll();
    }
    if (!data.isEmpty())
        emit receivedData(data.constData(), int(data.size()));
    // The end of the output comes after the process's (the console is closed
    // then), so nothing it wrote is lost.
    if (eof && !_finished) {
        _finished = true;
        emit finished(_exitCode, QProcess::NormalExit);
    }
}

void Pty::processExited()
{
    if (_exited)
        return;
    _exited = true;
    _exitNotifier->setEnabled(false);
    DWORD code = 0;
    GetExitCodeProcess(_state->process, &code);
    _exitCode = int(code);
    closeConsole(_state);
}

void Pty::setWindowSize(int lines, int cols)
{
    _windowColumns = cols;
    _windowLines = lines;
    if (_state && !_state->closing && lines > 0 && cols > 0)
        ResizePseudoConsole(_state->console, COORD{SHORT(cols), SHORT(lines)});
}

void Pty::sendData(const char *buffer, int length)
{
    if (!_state || _exited)
        return;
    while (length > 0) {
        DWORD written = 0;
        if (!WriteFile(_state->input, buffer, DWORD(length), &written, nullptr) || written == 0)
            return;
        buffer += written;
        length -= int(written);
    }
}

void Pty::closePty()
{
    if (_state)
        closeConsole(_state);
}

void Pty::kill()
{
    if (_state && !_exited)
        TerminateProcess(_state->process, 1);
}

QProcess::ProcessState Pty::state() const
{
    return _state && !_finished ? QProcess::Running : QProcess::NotRunning;
}

bool Pty::waitForFinished(int msecs)
{
    if (!_state || _exited)
        return true;
    return WaitForSingleObject(_state->process, msecs < 0 ? INFINITE : DWORD(msecs)) == WAIT_OBJECT_0;
}

int Pty::foregroundProcessGroup() const
{
    if (!_state || _exited)
        return 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return int(_processId);

    struct Entry { DWORD pid, parent; };
    QList<Entry> entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        // The console's own host is started under the shell by some programs.
        if (QString::fromWCharArray(entry.szExeFile).compare(QLatin1String("conhost.exe"), Qt::CaseInsensitive) != 0)
            entries.append({entry.th32ProcessID, entry.th32ParentProcessID});
    }
    CloseHandle(snapshot);

    // A parent id can be that of a process long gone, reused: one started
    // before its "parent" is not its child.
    auto created = [](DWORD pid) -> quint64 {
        quint64 when = 0;
        if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            FILETIME creation, exit, kernel, user;
            if (GetProcessTimes(process, &creation, &exit, &kernel, &user))
                when = (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
            CloseHandle(process);
        }
        return when;
    };

    DWORD newest = DWORD(_processId);
    quint64 newestTime = 0;
    QList<Entry> pending{{DWORD(_processId), 0}};
    QList<DWORD> seen{DWORD(_processId)};
    while (!pending.isEmpty()) {
        const DWORD parent = pending.takeFirst().pid;
        const quint64 parentTime = created(parent);
        for (const Entry &e : std::as_const(entries)) {
            if (e.parent != parent || seen.contains(e.pid))
                continue;
            const quint64 time = created(e.pid);
            if (time == 0 || time < parentTime)
                continue;
            seen.append(e.pid);
            pending.append(e);
            if (time >= newestTime) {
                newestTime = time;
                newest = e.pid;
            }
        }
    }
    return int(newest);
}
