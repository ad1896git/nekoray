#include "Renumber.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Renumber {
void Require(bool condition, const QString &message) {
    if (!condition) throw Error(message);
}
bool Exists(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}
bool Pending() { return Exists(".renumber"); }
QByteArray Read(const QString &path) {
    const QFileInfo info(path);
    Require(info.isFile() && !info.isSymLink(), "Not a regular file: " + path);
    QFile file(path);
    Require(file.open(QIODevice::ReadOnly), "Cannot read: " + path);
    const QByteArray bytes = file.readAll();
    Require(file.error() == QFileDevice::NoError, "Read failed: " + path);
    return bytes;
}
namespace {
const QStringList States{"PREPARE", "BACKUP_DONE", "STAGED", "COMMIT", "DONE"};

// Both file and directory synchronization use this single native helper.
// Process-crash recovery is the guarantee; device-level durability is unverified.
bool NativeSync(int fd) {
#ifdef Q_OS_WIN
    const intptr_t handle = _get_osfhandle(fd);
    return handle != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(handle));
#elif defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) == 0;
#else
    return ::fsync(fd) == 0;
#endif
}
QByteArray Hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
QByteArray Json(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QJsonObject Object(const QByteArray &bytes) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(bytes, &error);
    Require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid journal JSON");
    return document.object();
}
void SafePath(const QString &path) {
    Require(path.startsWith("profiles/") || path.startsWith("groups/"), "Invalid snapshot root: " + path);
    for (const QString &part : path.split('/')) {
        Require(!part.isEmpty() && part != "." && part != ".." && !part.contains('\\') &&
                !part.contains(':') && !part.contains(QChar(0)), "Invalid snapshot path: " + path);
    }
}
void Walk(const QString &path, Files &files, QSet<QString> &names) {
    const QFileInfo info(path);
    Require(!info.isSymLink(), "Symlink in data tree: " + path);
    const QString folded = path.toCaseFolded();
    Require(!names.contains(folded), "Ambiguous path: " + path);
    names.insert(folded);
    if (info.isDir()) {
        Require(info.isReadable(), "Unreadable directory: " + path);
        const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                     QDir::Hidden | QDir::System, QDir::Name);
        for (const auto &entry : entries) Walk(path + "/" + entry.fileName(), files, names);
        return;
    }
    SafePath(path);
    Require(info.isFile(), "Special file in data tree: " + path);
    files.insert(path, Read(path));
}
QJsonObject EncodeFiles(const Files &files) {
    QJsonObject entries;
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        SafePath(it.key());
        entries.insert(it.key(), QJsonObject{{"data", QString::fromLatin1(it.value().toBase64())},
                                            {"sha256", QString::fromLatin1(Hash(it.value()))}});
    }
    return QJsonObject{{"count", files.size()}, {"files", entries}};
}
Files DecodeFiles(const QJsonValue &value) {
    Require(value.isObject(), "Invalid snapshot");
    const auto object = value.toObject();
    Require(object.size() == 2 && object.value("files").isObject() && object.value("count").isDouble(),
            "Invalid snapshot fields");
    const auto entries = object.value("files").toObject();
    Require(object.value("count").toDouble() == double(entries.size()), "Snapshot count mismatch");
    Files files;
    QSet<QString> names;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        SafePath(it.key());
        const auto folded = it.key().toCaseFolded();
        Require(!names.contains(folded), "Ambiguous snapshot path");
        names.insert(folded);
        Require(it.value().isObject(), "Invalid snapshot entry");
        const auto entry = it.value().toObject();
        Require(entry.size() == 2 && entry.value("data").isString() && entry.value("sha256").isString(),
                "Invalid snapshot entry fields");
        const auto encoded = entry.value("data").toString().toLatin1();
        const auto bytes = QByteArray::fromBase64(encoded);
        Require(bytes.toBase64() == encoded, "Invalid base64");
        Require(Hash(bytes) == entry.value("sha256").toString().toLatin1(), "Snapshot hash mismatch");
        files.insert(it.key(), bytes);
    }
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        QString parent = QFileInfo(it.key()).path();
        while (parent.contains('/')) {
            Require(!names.contains(parent.toCaseFolded()), "File/directory collision in snapshot");
            parent = QFileInfo(parent).path();
        }
    }
    return files;
}
struct Bundle { Files oldFiles; Files newFiles; QByteArray digest; };
QByteArray EncodeBundle(const Files &oldFiles, const Files &newFiles) {
    const auto payload = Json(QJsonObject{{"version", 1}, {"old", EncodeFiles(oldFiles)}, {"new", EncodeFiles(newFiles)}});
    return Json(QJsonObject{{"payload", QString::fromLatin1(payload.toBase64())},
                            {"sha256", QString::fromLatin1(Hash(payload))}});
}
Bundle ReadBundle(const QString &directory) {
    const auto bytes = Read(directory + "/bundle.json");
    const auto envelope = Object(bytes);
    Require(envelope.size() == 2 && envelope.value("payload").isString() && envelope.value("sha256").isString(),
            "Invalid bundle envelope");
    const auto encoded = envelope.value("payload").toString().toLatin1();
    const auto payload = QByteArray::fromBase64(encoded);
    Require(payload.toBase64() == encoded && Hash(payload) == envelope.value("sha256").toString().toLatin1(),
            "Bundle checksum mismatch");
    const auto object = Object(payload);
    Require(object.size() == 3 && object.value("version").isDouble() && object.value("version").toDouble() == 1.0,
            "Unsupported journal version");
    return {DecodeFiles(object.value("old")), DecodeFiles(object.value("new")), Hash(bytes)};
}
class Ops {
public:
    explicit Ops(const Hooks &hooks) : hooks_(hooks) {}
    void Point(const QString &name) const { if (hooks_.point) hooks_.point(name); }
    void Do(const QString &name, const std::function<bool()> &operation) {
        Point(name + ":before");
        const bool failed = hooks_.fail && hooks_.fail(name);
        Require(!failed && operation(), "Persistence failure: " + name);
        Point(name + ":after");
    }
    void SyncDirectory(const QString &path, const QString &label) {
        Do("dirsync:" + label, [&] {
#ifdef Q_OS_WIN
            // File flushes and write-through moves are checked; directory persistence is unverified.
            Q_UNUSED(path);
            return true;
#else
            const auto native = QFile::encodeName(path);
            const int fd = ::open(native.constData(), O_RDONLY);
            if (fd < 0) return false;
            const bool ok = NativeSync(fd);
            const bool closed = ::close(fd) == 0;
            return ok && closed;
#endif
        });
    }
    void Move(const QString &from, const QString &to, const QString &label, bool replace) {
        Require(replace || !Exists(to), "Destination occupied: " + to);
        Do("rename:" + label, [&] {
#ifdef Q_OS_WIN
            const auto a = QDir::toNativeSeparators(from);
            const auto b = QDir::toNativeSeparators(to);
            DWORD flags = MOVEFILE_WRITE_THROUGH;
            if (replace) flags |= MOVEFILE_REPLACE_EXISTING;
            return MoveFileExW(reinterpret_cast<LPCWSTR>(a.utf16()), reinterpret_cast<LPCWSTR>(b.utf16()), flags) != 0;
#else
            const auto a = QFile::encodeName(from);
            const auto b = QFile::encodeName(to);
            return ::rename(a.constData(), b.constData()) == 0;
#endif
        });
        SyncDirectory(QFileInfo(from).path(), label + ":source");
        if (QFileInfo(from).path() != QFileInfo(to).path()) SyncDirectory(QFileInfo(to).path(), label + ":destination");
    }
    void MakeDirectory(const QString &path, const QString &label) {
        Do("mkdir:" + label, [&] { return QDir().mkdir(path); });
#ifndef Q_OS_WIN
        Do("permissions:" + label, [&] { return QFile::setPermissions(path,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); });
#endif
        SyncDirectory(QFileInfo(path).path(), label + ":parent");
    }
    void Write(const QString &directory, const QString &destination, const QByteArray &bytes, const QString &label) {
        const QString temporary = directory + "/io.tmp";
        if (Exists(temporary)) {
            const QFileInfo info(temporary);
            Require(info.isFile() && !info.isSymLink(), "Invalid transaction temporary file");
        }
        QFile file(temporary);
        Do("open:" + label, [&] { return file.open(QIODevice::WriteOnly | QIODevice::Truncate); });
#ifndef Q_OS_WIN
        Do("permissions:" + label, [&] { return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
#endif
        const auto half = bytes.size() / 2;
        Do("write-head:" + label, [&] { return file.write(bytes.constData(), half) == half; });
        Do("write-tail:" + label, [&] { return file.write(bytes.constData() + half, bytes.size() - half) == bytes.size() - half; });
        Do("flush:" + label, [&] { return file.flush(); });
        Do("filesync:" + label, [&] { return NativeSync(file.handle()); });
        file.close();
        Require(file.error() == QFileDevice::NoError, "Close failed: " + temporary);
        Move(temporary, destination, label, true);
    }
    void Remove(const QString &path) {
        Do("unlink:" + path, [&] { return QFile::remove(path); });
        SyncDirectory(QFileInfo(path).path(), "unlink:" + path);
        Point("removed:" + path);
    }
private:
    Hooks hooks_;
};
int ReadState(const Bundle &bundle) {
    const QFileInfo info(".renumber");
    Require(info.isDir() && !info.isSymLink(), "Invalid active journal directory");
    const auto entries = QDir(".renumber").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &entry : entries) {
        Require(entry == "bundle.json" || entry == "io.tmp" || States.contains(entry), "Unknown journal entry");
        const QFileInfo child(".renumber/" + entry);
        Require(child.isFile() && !child.isSymLink(), "Invalid journal entry");
    }
    int state = -1;
    bool missing = false;
    for (int i = 0; i < States.size(); ++i) {
        const auto path = ".renumber/" + States.at(i);
        if (!Exists(path)) { missing = true; continue; }
        Require(!missing, "Noncontiguous journal states");
        Require(Read(path) == bundle.digest + '\n', "Invalid state marker");
        state = i;
    }
    Require(state >= 0, "Missing PREPARE marker");
    return state;
}
void Mark(Ops &ops, const QString &directory, const QString &state, const QByteArray &digest) {
    ops.Write(directory, directory + "/" + state, digest + '\n', state);
    ops.Point("state:" + state);
}
void Archive(Ops &ops, bool completed) {
    const auto destination = (completed ? ".renumber-done-" : ".renumber-aborted-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    ops.Move(".renumber", destination, "archive", false);
    ops.Point(completed ? "archived:done" : "archived:aborted");
}
void VerifyPartial(const Files &live, const Bundle &bundle) {
    for (auto it = live.constBegin(); it != live.constEnd(); ++it) {
        const bool oldMatch = bundle.oldFiles.contains(it.key()) && bundle.oldFiles.value(it.key()) == it.value();
        const bool newMatch = bundle.newFiles.contains(it.key()) && bundle.newFiles.value(it.key()) == it.value();
        Require(oldMatch || newMatch, "Unexpected live contents: " + it.key());
    }
    for (auto it = bundle.oldFiles.constBegin(); it != bundle.oldFiles.constEnd(); ++it) {
        if (bundle.newFiles.contains(it.key())) Require(live.contains(it.key()), "Missing common live file: " + it.key());
    }
}
} // namespace
Files Scan() {
    Files files;
    QSet<QString> names;
    for (const auto &root : {QString("profiles"), QString("groups")}) {
        Require(QFileInfo(root).isDir() && !QFileInfo(root).isSymLink(), "Missing or invalid data directory: " + root);
        Walk(root, files, names);
    }
    return files;
}
void Lease::Lock(int byte, bool exclusive, bool unlock) {
#ifdef Q_OS_WIN
    const intptr_t native = _get_osfhandle(file_.handle());
    Require(native != -1, "Cannot obtain configuration lock handle");
    HANDLE handle = reinterpret_cast<HANDLE>(native);
    OVERLAPPED overlap{};
    overlap.Offset = DWORD(byte);
    const bool ok = unlock ? UnlockFileEx(handle, 0, 1, 0, &overlap) != 0
        : LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &overlap) != 0;
#else
    struct flock lock{};
    lock.l_type = unlock ? F_UNLCK : (exclusive ? F_WRLCK : F_RDLCK);
    lock.l_whence = SEEK_SET;
    lock.l_start = byte;
    lock.l_len = 1;
    const bool ok = ::fcntl(file_.handle(), F_SETLK, &lock) == 0;
#endif
    Require(ok, "Configuration is in use; close other instances and retry");
}
bool Lease::Begin(bool reorder) {
    Require(!file_.isOpen(), "Configuration lease already acquired");
    if (Exists(".renumber.lock")) {
        const QFileInfo info(".renumber.lock");
        Require(info.isFile() && !info.isSymLink(), "Invalid configuration lock file");
    }
    file_.setFileName(".renumber.lock");
    Require(file_.open(QIODevice::ReadWrite), "Cannot open configuration lock");
#ifdef Q_OS_WIN
    const intptr_t native = _get_osfhandle(file_.handle());
    Require(native != -1 && SetHandleInformation(reinterpret_cast<HANDLE>(native), HANDLE_FLAG_INHERIT, 0),
            "Cannot protect configuration lock handle");
#else
    Require(::fcntl(file_.handle(), F_SETFD, FD_CLOEXEC) == 0, "Cannot protect configuration lock descriptor");
#endif
    Lock(0, true);
    gate_ = true;
    exclusive_ = reorder || Pending();
    Lock(1, exclusive_);
    return exclusive_;
}
void Lease::Ready() {
    Require(gate_, "Missing startup gate");
    if (exclusive_) {
        // Keep the startup gate during the Windows unlock/relock conversion.
        Lock(1, true, true);
        Lock(1, false);
        exclusive_ = false;
    }
    Lock(0, true, true);
    gate_ = false;
}
void Recover(Lease &lease, const Hooks &hooks) {
    Require(lease.Exclusive(), "Recovery requires exclusive ownership");
    if (!Pending()) return;
    Ops ops(hooks);
    const auto bundle = ReadBundle(".renumber");
    const int state = ReadState(bundle);
    ops.Point("recovery:validated");
    if (state < States.indexOf("COMMIT")) {
        Require(Scan() == bundle.oldFiles, "Precommit live data differs from backup");
        Archive(ops, false);
        return;
    }
    if (state == States.indexOf("DONE")) {
        Require(Scan() == bundle.newFiles, "Completed transaction differs from live data");
        Archive(ops, true);
        return;
    }
    ops.SyncDirectory(".renumber", "commit");
    ops.SyncDirectory(".", "commit-parent");
    const auto live = Scan();
    VerifyPartial(live, bundle);
    for (auto it = bundle.newFiles.constBegin(); it != bundle.newFiles.constEnd(); ++it) {
        if (!live.contains(it.key()) || live.value(it.key()) != it.value())
            ops.Write(".renumber", it.key(), it.value(), "install:" + it.key());
        else ops.SyncDirectory(QFileInfo(it.key()).path(), "existing:" + it.key());
        ops.Point("installed:" + it.key());
    }
    // Every replacement exists before removing any obsolete filename.
    for (auto it = bundle.oldFiles.constBegin(); it != bundle.oldFiles.constEnd(); ++it) {
        if (!bundle.newFiles.contains(it.key()) && Exists(it.key())) ops.Remove(it.key());
    }
    Require(Scan() == bundle.newFiles, "Installed snapshot verification failed");
    ops.SyncDirectory("profiles", "profiles-final");
    ops.SyncDirectory("groups", "groups-final");
    Mark(ops, ".renumber", "DONE", bundle.digest);
    Archive(ops, true);
}
void Start(Lease &lease, const Files &oldFiles, const Files &newFiles, const Hooks &hooks) {
    Require(lease.Exclusive(), "Renumbering requires exclusive ownership");
    Require(!Pending(), "Unfinished renumbering already exists");
    Require(Scan() == oldFiles, "Source changed during validation");
    Ops ops(hooks);
    const auto directory = ".renumber-build-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    ops.MakeDirectory(directory, "build");
    const auto encoded = EncodeBundle(oldFiles, newFiles);
    ops.Write(directory, directory + "/bundle.json", encoded, "bundle");
    Mark(ops, directory, "PREPARE", Hash(encoded));
    ops.Move(directory, ".renumber", "publish", false);
    auto bundle = ReadBundle(".renumber");
    Require(bundle.oldFiles == oldFiles && bundle.newFiles == newFiles, "Backup readback failed");
    Mark(ops, ".renumber", "BACKUP_DONE", bundle.digest);
    bundle = ReadBundle(".renumber");
    Require(bundle.oldFiles == oldFiles && bundle.newFiles == newFiles, "Staging readback failed");
    Mark(ops, ".renumber", "STAGED", bundle.digest);
    Require(Scan() == oldFiles, "Source changed before commit");
    bundle = ReadBundle(".renumber");
    Require(bundle.oldFiles == oldFiles && bundle.newFiles == newFiles, "Final bundle verification failed");
    Mark(ops, ".renumber", "COMMIT", bundle.digest);
    Recover(lease, hooks);
}
} // namespace Renumber
