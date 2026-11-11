#pragma once

#include <QString>
#include <QByteArray>
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>

namespace NekoGui_sys::StaleCore {
struct Identity {
    qint64 pid = 0;
    QString start;
    QString executable;
    QString clockBase; // Linux owner only: boot ID and recording time namespace.
};
struct Record {
    Identity process;
    Identity owner; // GUI pid/start, observation only; never a signal target.
    QByteArray token;
    int port = 0;
    QString config;
};
QString recordPath();
Identity observe(qint64 pid);
enum class OwnerState { MATCHING_LIVE, CONFIRMED_DEAD, CONFIRMED_REUSED, UNKNOWN };
// PID/start observation only; executable lookup and signaling are unnecessary.
OwnerState observeOwner(const Identity &expected);
bool same(const Identity &expected, const Identity &actual);
bool read(const QString &path, Record &record);
bool write(const QString &path, const Record &record);
void removeOwned(const QString &path, const QByteArray &token);

// Blocking implementation, called only on a dedicated worker. There is no
// stale-PID signal operation: authentication binds Exit to the previous launch.
struct Operations {
    std::function<Identity(qint64)> observeProcess = observe;
    std::function<OwnerState(const Identity &)> ownerState = observeOwner;
    std::function<void(const Record &)> exit;
    // 0: connection refused; 1: listener present; -1: cannot establish absence.
    std::function<int(int)> listener;
};
bool wait(const QString &path, const std::atomic_bool &cancelled,
          Operations operations = {}, int boundMs = 12000, QString *refusal = nullptr);
std::shared_ptr<std::atomic_bool> start(QObject *context, const QString &path,
                                      std::function<void(bool, QString)> completed);
}
