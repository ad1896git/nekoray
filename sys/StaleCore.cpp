#include "StaleCore.hpp"
#include "rpc/RpcTransport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <climits>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <libproc.h>
#endif

namespace NekoGui_sys::StaleCore {
QString recordPath() { return QDir::current().absoluteFilePath(".core-launch.json"); }

Identity observe(qint64 pid) {
    Identity result;
    if (pid <= 0 || pid > INT_MAX) return result;
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    if (proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info) ||
        info.pbi_status == 5 || proc_pidpath(int(pid), path, sizeof(path)) <= 0) return result;
    result = {pid, QString::number(info.pbi_start_tvsec) + ":" + QString::number(info.pbi_start_tvusec),
              QString::fromLocal8Bit(path)};
#elif defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return result;
    const auto data = stat.readAll();
    const auto fields = data.mid(data.lastIndexOf(')') + 2).split(' ');
    if (fields.size() < 20 || fields[0] == "Z") return result;
    const auto exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
    if (exe.isEmpty()) return result;
    result = {pid, QString::fromLatin1(fields[19]), exe};
#endif
    return result;
}

OwnerState observeOwner(const Identity &expected) {
    if (expected.pid <= 0 || expected.pid > INT_MAX || expected.start.isEmpty())
        return OwnerState::UNKNOWN;
    QString start;
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    errno = 0;
    const int bytes = proc_pidinfo(int(expected.pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (bytes != sizeof(info)) {
        // A short read or an access error is not evidence of process death.
        return bytes <= 0 && errno == ESRCH ? OwnerState::CONFIRMED_DEAD : OwnerState::UNKNOWN;
    }
    if (info.pbi_pid != expected.pid) return OwnerState::UNKNOWN;
    if (info.pbi_status == 5) return OwnerState::CONFIRMED_DEAD; // zombie
    if (info.pbi_status < 1 || info.pbi_status > 5 || !info.pbi_start_tvsec || info.pbi_start_tvusec >= 1000000)
        return OwnerState::UNKNOWN;
    start = QString::number(info.pbi_start_tvsec) + ":" + QString::number(info.pbi_start_tvusec);
#elif defined(Q_OS_LINUX)
    const auto name = QFile::encodeName(QStringLiteral("/proc/%1/stat").arg(expected.pid));
    const int fd = ::open(name.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT || errno == ESRCH ? OwnerState::CONFIRMED_DEAD : OwnerState::UNKNOWN;
    QFile stat;
    if (!stat.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return OwnerState::UNKNOWN;
    }
    const auto data = stat.readAll();
    const auto end = data.lastIndexOf(')');
    if (stat.error() != QFileDevice::NoError || !data.endsWith('\n') ||
        !data.startsWith(QByteArray::number(expected.pid) + " (") || end < 0 || data.mid(end, 2) != ") ")
        return OwnerState::UNKNOWN;
    const auto fields = data.mid(end + 2).simplified().split(' ');
    if (fields.size() < 20 || fields[0].size() != 1 || !QByteArray("RSDZTtWXxKPI").contains(fields[0]))
        return OwnerState::UNKNOWN;
    bool validStart = false;
    fields[19].toULongLong(&validStart);
    if (!validStart) return OwnerState::UNKNOWN;
    if (fields[0] == "Z") return OwnerState::CONFIRMED_DEAD;
    start = QString::fromLatin1(fields[19]);
#else
    return OwnerState::UNKNOWN;
#endif
    return start == expected.start ? OwnerState::MATCHING_LIVE : OwnerState::CONFIRMED_REUSED;
}

bool same(const Identity &expected, const Identity &actual) {
    return expected.pid > 0 && !expected.start.isEmpty() && !expected.executable.isEmpty() &&
           expected.pid == actual.pid && expected.start == actual.start &&
           expected.executable == actual.executable;
}

bool read(const QString &path, Record &record) {
    QFile file;
#ifdef Q_OS_UNIX
    const auto name = QFile::encodeName(path);
    const int fd = ::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 0777) != 0600 || st.st_size <= 0 || st.st_size > 4096) {
        ::close(fd);
        return false;
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
#else
    return false;
#endif
    const auto bytes = file.read(4097);
    if (bytes.size() > 4096) return false;
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return false;
    const auto object = document.object();
    Record value;
    value.token = object["token"].toString().toLatin1();
    value.port = object["port"].toInt();
    value.config = object["config"].toString();
    value.process = {object["pid"].toString().toLongLong(), object["start"].toString(),
                     object["exe"].toString()};
    if (object["version"].toInt() != 1 || value.config != QFileInfo(path).absolutePath() ||
        value.token.size() != 64 ||
        !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(QString::fromLatin1(value.token)).hasMatch() ||
        value.port < 1 || value.port > 65535 || value.process.pid < 0 || value.process.pid > INT_MAX ||
        (value.process.pid > 0 && (value.process.start.isEmpty() || value.process.executable.isEmpty())))
        return false;
    const auto owner = object["owner"].toObject();
    value.owner = {owner["pid"].toString().toLongLong(), owner["start"].toString(), {}};
    // Legacy/missing/incomplete owner metadata retains the authenticated,
    // bounded recovery path; it must not invalidate an otherwise valid record.
    if (value.owner.pid <= 0 || value.owner.pid > INT_MAX || value.owner.start.isEmpty())
        value.owner = {};
    record = value;
    return true;
}

bool write(const QString &path, const Record &record) {
#ifdef Q_OS_UNIX
    const QJsonObject object{{"version", 1}, {"token", QString::fromLatin1(record.token)},
        {"port", record.port}, {"config", record.config},
        {"pid", QString::number(record.process.pid)}, {"start", record.process.start},
        {"exe", record.process.executable},
        {"owner", QJsonObject{{"pid", QString::number(record.owner.pid)}, {"start", record.owner.start}}}};
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
#else
    return true;
#endif
}

void removeOwned(const QString &path, const QByteArray &token) {
    Record record;
    if (read(path, record) && record.token == token) QFile::remove(path);
}

namespace {
int listener(int port) {
#ifdef Q_OS_UNIX
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) || ::fcntl(fd, F_SETFL, O_NONBLOCK)) {
        ::close(fd);
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(quint16(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int error = 0;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        error = errno;
        if (error == EINPROGRESS) {
            pollfd pending{fd, POLLOUT, 0};
            if (::poll(&pending, 1, 40) > 0) {
                socklen_t length = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
            }
        }
    }
    ::close(fd);
    return error == 0 ? 1 : error == ECONNREFUSED ? 0 : -1;
#else
    Q_UNUSED(port);
    return -1;
#endif
}
void exit(const Record &record) {
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/libcore.LibcoreService/Exit").arg(record.port)));
    request.setAttribute(QNetworkRequest::Http2DirectAttribute, true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/grpc");
    request.setRawHeader("te", "trailers");
    request.setRawHeader("nekoray_auth", record.token);
    NkrW04::RpcTransport transport;
    // EmptyReq is the empty protobuf message, in the existing unary gRPC frame.
    transport.send(request, QByteArray(5, '\0'), 500);
}
}

bool wait(const QString &path, const std::atomic_bool &cancelled, Operations operations, int boundMs, QString *refusal) {
#ifdef Q_OS_UNIX
    if (refusal) refusal->clear();
    Record record;
    if (!read(path, record)) {
        if (QFile::exists(path)) {
            qWarning("Startup: ignoring invalid core launch record");
            QFile::remove(path); // removes a symlink itself, never its target
        }
        return !cancelled.load();
    }
    if (!operations.exit) operations.exit = exit;
    if (!operations.listener) operations.listener = listener;
    QElapsedTimer timer;
    timer.start();
    if (cancelled.load()) return false;
    if (record.owner.pid && record.owner.pid != QCoreApplication::applicationPid()) {
        const auto owner = operations.ownerState(record.owner);
        if (owner == OwnerState::MATCHING_LIVE) {
            if (refusal) *refusal = QCoreApplication::translate("MainWindow",
                "Another NekoBox GUI is using this configuration. Close it or use a different -appdata directory, then restart NekoBox.");
            qWarning("Startup refused: Another NekoBox GUI is using this configuration");
            return false; // Preserve the incumbent record and core; do not send Exit.
        }
        if (owner != OwnerState::CONFIRMED_DEAD && owner != OwnerState::CONFIRMED_REUSED) {
            if (refusal) *refusal = QCoreApplication::translate("MainWindow",
                "Cannot determine whether another NekoBox GUI is using this configuration. Close other instances or use a different -appdata directory, then restart NekoBox.");
            qWarning("Startup refused: Cannot determine whether another NekoBox GUI is using this configuration");
            return false; // Unknown must preserve the record and must not send Exit.
        }
    }
    qInfo("Startup: waiting for previous core launch");
    operations.exit(record);
    do {
        if (cancelled.load()) return false;
        const bool alive = record.process.pid && same(record.process, operations.observeProcess(record.process.pid));
        const int port = operations.listener(record.port);
        // A record published before spawn has no PID yet. Give the previous
        // child's listener/token initialization the entire parent-poll budget;
        // immediate connection refusal alone is not proof of spawn failure.
        if (!alive && port == 0 && (record.process.pid || timer.elapsed() >= boundMs)) {
            removeOwned(path, record.token);
            qInfo("Startup: previous core launch exited");
            return true;
        }
        if (timer.elapsed() >= boundMs) break;
        QThread::msleep(25);
    } while (true);
    const auto observed = operations.observeProcess(record.process.pid);
    qWarning("Startup refused: previous core exit could not be confirmed (identity_alive=%d listener=%d)",
             int(same(record.process, observed)), operations.listener(record.port));
    return false;
#else
    Q_UNUSED(path); Q_UNUSED(operations); Q_UNUSED(boundMs); Q_UNUSED(refusal);
    return !cancelled.load();
#endif
}

std::shared_ptr<std::atomic_bool> start(QObject *context, const QString &path,
                                      std::function<void(bool, QString)> completed) {
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    QObject::connect(context, &QObject::destroyed, [cancelled] { cancelled->store(true); });
    auto result = std::make_shared<bool>(false);
    auto refusal = std::make_shared<QString>();
    auto *worker = QThread::create([path, cancelled, result, refusal] {
        *result = wait(path, *cancelled, {}, 12000, refusal.get());
    });
    QObject::connect(worker, &QThread::finished, context, [completed, result, refusal] { completed(*result, *refusal); });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
    return cancelled;
}
}
