#pragma once

#include "StaleCore.hpp"
#include <QRegularExpression>

// The reader is injectable so Linux clock-base decisions can also be tested
// on hosts without procfs or permission to create time namespaces.
namespace NekoGui_sys::StaleCore::LinuxOwner {
struct ReadResult {
    QByteArray data;
    bool missing = false; // Only ENOENT/ESRCH from opening the stat file.
};
using Reader = std::function<ReadResult(const QString &)>;

inline QString clockBase(qint64 pid, const Reader &read) {
    const auto ours = read(QStringLiteral("/proc/thread-self/ns/time")).data;
    const auto theirs = read(QStringLiteral("/proc/%1/ns/time").arg(pid)).data;
    const auto boot = read(QStringLiteral("/proc/sys/kernel/random/boot_id")).data.trimmed();
    static const QRegularExpression ns(QStringLiteral("^time:\\[[0-9]+\\]$"));
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    if (ours != theirs || !ns.match(QString::fromLatin1(ours)).hasMatch() ||
        !uuid.match(QString::fromLatin1(boot)).hasMatch()) return {};
    return QString::fromLatin1(boot + '/' + ours);
}

struct Sample {
    Identity identity;
    OwnerState state = OwnerState::UNKNOWN;
};

inline bool validStart(const QString &start) {
    bool valid = false;
    const auto ticks = start.toULongLong(&valid);
    // Require the complete, in-range canonical decimal spelling emitted by procfs.
    return valid && QString::number(ticks) == start;
}

inline Sample sample(qint64 pid, const Reader &read) {
    Sample result;
    const auto before = clockBase(pid, read);
    const auto stat = read(QStringLiteral("/proc/%1/stat").arg(pid));
    if (stat.missing) {
        result.state = OwnerState::CONFIRMED_DEAD;
        return result;
    }
    const auto &data = stat.data;
    const auto end = data.lastIndexOf(')');
    if (!data.endsWith('\n') || !data.startsWith(QByteArray::number(pid) + " (") ||
        end < 0 || data.mid(end, 2) != ") ") return result;
    const auto fields = data.mid(end + 2).simplified().split(' ');
    if (fields.size() < 20 || fields[0].size() != 1 || !QByteArray("RSDZTtWXxKPI").contains(fields[0]))
        return result;
    const auto start = QString::fromLatin1(fields[19]);
    if (!validStart(start)) return result;
    // Preserve PID/start even without a clock base: an unobservable live owner
    // must not become a missing/legacy owner that bypasses the startup gate.
    result.identity = {pid, start, {}};
    // A validated zombie is definitively dead even after its namespaces vanish.
    if (fields[0] == "Z") {
        result.state = OwnerState::CONFIRMED_DEAD;
        return result;
    }
    const auto after = clockBase(pid, read);
    if (before.isEmpty() || before != after) return result;
    result.identity.clockBase = before;
    result.state = OwnerState::MATCHING_LIVE;
    return result;
}

inline OwnerState observeOwner(const Identity &expected, const Reader &read) {
    if (!validStart(expected.start)) return OwnerState::UNKNOWN;
    const auto actual = sample(expected.pid, read);
    if (actual.state != OwnerState::MATCHING_LIVE) return actual.state;
    // Raw legacy ticks, a different boot, and a different recording namespace
    // are all ambiguous. Only compare ticks after proving the same clock base.
    if (expected.clockBase.isEmpty() || expected.clockBase != actual.identity.clockBase)
        return OwnerState::UNKNOWN;
    return expected.start == actual.identity.start ? OwnerState::MATCHING_LIVE : OwnerState::CONFIRMED_REUSED;
}
}
