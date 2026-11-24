/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "disclaim.h"

#include <QCoreApplication>

#ifdef Q_OS_MACOS
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

// Private, but what Terminal, iTerm2 and Chromium use; weak, so a macOS
// without it still runs the command (without the disclaimer).
extern "C" int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t *attrs, int disclaim)
    __attribute__((weak_import));

namespace {
constexpr char kFlag[] = "--puddlejumper-disclaim";
}
#endif

QStringList disclaimedCommand(const QStringList &command)
{
#ifdef Q_OS_MACOS
    if (!command.isEmpty())
        return QStringList{QCoreApplication::applicationFilePath(), QString::fromLatin1(kFlag)} + command;
#endif
    return command;
}

void runDisclaimedIfAsked(int argc, char **argv)
{
#ifdef Q_OS_MACOS
    if (argc < 3 || std::strcmp(argv[1], kFlag) != 0)
        return;
    // Become the command (POSIX_SPAWN_SETEXEC: in this process, with its
    // pipes), responsible for itself and what it starts.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
    if (responsibility_spawnattrs_setdisclaim)
        responsibility_spawnattrs_setdisclaim(&attr, 1);
    pid_t pid = 0;
    posix_spawnp(&pid, argv[2], nullptr, &attr, argv + 2, environ);
    // Still here: it didn't work that way. Plainly, then.
    execvp(argv[2], argv + 2);
    std::fprintf(stderr, "%s: %s\n", argv[2], std::strerror(errno));
    _exit(127);
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
#endif
}
