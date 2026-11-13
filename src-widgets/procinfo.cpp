/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#include "procinfo.h"

#include <QByteArray>
#include <QFile>

#if defined(Q_OS_MACOS)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <cstring>
#endif

namespace procinfo {

#if defined(Q_OS_LINUX)

namespace {
QByteArray readProc(int pid, const char *entry)
{
    QFile f(QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(entry)));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

QString name(int pid)
{
    // /proc/<pid>/comm is the kernel's short name for the process. Read it
    // directly rather than shelling out to ps.
    return pid > 0 ? QString::fromUtf8(readProc(pid, "comm")).trimmed() : QString();
}

QStringList arguments(int pid)
{
    QStringList args;
    if (pid <= 0)
        return args;
    for (const QByteArray &arg : readProc(pid, "cmdline").split('\0')) {
        if (!arg.isEmpty())
            args << QString::fromLocal8Bit(arg);
    }
    return args;
}

int terminalForegroundGroup(int pid)
{
    if (pid <= 0)
        return -1;
    // The command name in field 2 may contain spaces and parentheses, so
    // count fields from the last ')'. tpgid is field 8, the sixth after it.
    const QByteArray line = readProc(pid, "stat");
    const int close = line.lastIndexOf(')');
    if (close < 0)
        return -1;
    bool ok = false;
    const int tpgid = line.mid(close + 2).split(' ').value(5).toInt(&ok);
    return ok ? tpgid : -1;
}

#elif defined(Q_OS_MACOS)

QString name(int pid)
{
    char buf[2 * MAXCOMLEN + 1] = {};
    if (pid <= 0 || proc_name(pid, buf, sizeof(buf)) <= 0)
        return QString();
    return QString::fromUtf8(buf);
}

QStringList arguments(int pid)
{
    if (pid <= 0)
        return {};

    int argmax = 0;
    size_t size = sizeof(argmax);
    int mibMax[] = {CTL_KERN, KERN_ARGMAX};
    if (sysctl(mibMax, 2, &argmax, &size, nullptr, 0) != 0 || argmax <= 0)
        return {};

    // KERN_PROCARGS2: argc as an int, the executable's path, NUL padding,
    // then argc NUL-terminated arguments (followed by the environment).
    QByteArray buf(argmax, '\0');
    size = size_t(argmax);
    int mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
    if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
        return {};

    int argc = 0;
    std::memcpy(&argc, buf.constData(), sizeof(argc));
    const char *p = buf.constData() + sizeof(argc);
    const char *end = buf.constData() + size;

    p = static_cast<const char *>(std::memchr(p, '\0', end - p));   // skip exec path
    if (!p)
        return {};
    while (p < end && *p == '\0')
        ++p;

    QStringList args;
    while (p < end && args.size() < argc) {
        const char *next = static_cast<const char *>(std::memchr(p, '\0', end - p));
        if (!next)
            break;
        args << QString::fromLocal8Bit(p, int(next - p));
        p = next + 1;
    }
    return args;
}

int terminalForegroundGroup(int pid)
{
    if (pid <= 0)
        return -1;
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0)
        return -1;
    const int tpgid = info.kp_eproc.e_tpgid;
    return tpgid > 0 ? tpgid : -1;
}

#else

QString name(int) { return QString(); }
QStringList arguments(int) { return {}; }
int terminalForegroundGroup(int) { return -1; }

#endif

} // namespace procinfo
