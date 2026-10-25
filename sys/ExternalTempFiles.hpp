#pragma once

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace NekoGui_sys {

#ifdef Q_OS_UNIX
// Injectable operations are per object, never global.
// closeFile must consume the descriptor even when reporting failure.
struct TempFileOps {
    int (*chmodFile)(int, mode_t) = ::fchmod;
    ssize_t (*writeFile)(int, const void *, size_t) = ::write;
    int (*closeFile)(int) = ::close;
};
#endif

class ExternalTempFiles final {
public:
#ifdef Q_OS_UNIX
    explicit ExternalTempFiles(
        const QString &root = QDir::currentPath(),
        TempFileOps ops = {})
        : root_(QDir(root).absolutePath()), ops_(ops) {}
#else
    explicit ExternalTempFiles(const QString &root = QDir::currentPath())
        : root_(QDir(root).absolutePath()) {}
#endif

    ExternalTempFiles(const ExternalTempFiles &) = delete;
    ExternalTempFiles &operator=(const ExternalTempFiles &) = delete;

    ~ExternalTempFiles() {
        clear();
#ifdef Q_OS_UNIX
        if (directory_ >= 0) ::close(directory_);
#endif
    }

    bool write(const QString &name, const QByteArray &data,
               QString &path, QString &error) {
        path.clear();
        error.clear();

        if (!validName(name))
            return fail(error, "invalid filename");
        if (!prepareDirectory(error))
            return false;

#ifdef Q_OS_UNIX
        const QByteArray encoded = QFile::encodeName(name);
        const int fd = ::openat(
            directory_, encoded.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return fail(error, "exclusive creation");

        struct stat st {};
        bool ok = ops_.chmodFile(fd, 0600) == 0;
        if (ok) {
            ok = ::fstat(fd, &st) == 0 &&
                 S_ISREG(st.st_mode) &&
                 st.st_uid == ::geteuid() &&
                 st.st_nlink == 1 &&
                 (st.st_mode & 07777) == 0600;
        }

        qint64 offset = 0;
        while (ok && offset < data.size()) {
            const size_t count = static_cast<size_t>(
                std::min<qint64>(data.size() - offset, 1024 * 1024));
            const ssize_t written =
                ops_.writeFile(fd, data.constData() + offset, count);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0 || static_cast<size_t>(written) > count) {
                ok = false;
                break;
            }
            offset += written;
        }

        // Do not retry close: a failed close may already have consumed fd.
        if (ops_.closeFile(fd) != 0)
            ok = false;

        if (!ok) {
            if (::unlinkat(directory_, encoded.constData(), 0) != 0 &&
                errno != ENOENT) {
                qWarning("Cannot remove incomplete external-core temporary file.");
            }
            return fail(error, "permissions, write, or close");
        }

        entries_.append(Entry{name, st.st_dev, st.st_ino});
#else
        const QString absolute = QDir(root_).filePath("temp/" + name);
        QFile file(absolute);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return fail(error, "exclusive creation");

        bool ok = true;
        qint64 offset = 0;
        while (offset < data.size()) {
            const qint64 written =
                file.write(data.constData() + offset, data.size() - offset);
            if (written <= 0) {
                ok = false;
                break;
            }
            offset += written;
        }
        if (!file.flush())
            ok = false;
        file.close();
        if (file.error() != QFileDevice::NoError)
            ok = false;

        if (!ok) {
            if (!file.remove())
                qWarning("Cannot remove incomplete external-core temporary file.");
            return fail(error, "write or close");
        }
        entries_.append(Entry{name});
#endif

        path = QDir(root_).filePath("temp/" + name);
        return true;
    }

    // Idempotent. Retain failed removals so later callbacks/destruction retry.
    void clear() {
        QVector<Entry> remaining;
        for (const Entry &entry : entries_) {
#ifdef Q_OS_UNIX
            const QByteArray encoded = QFile::encodeName(entry.name);
            struct stat st {};
            if (::fstatat(directory_, encoded.constData(), &st,
                          AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT)
                    continue;
                remaining.append(entry);
                qWarning("Cannot inspect external-core temporary file for cleanup.");
                continue;
            }

            // Do not delete a replacement that this object did not create.
            if (st.st_dev != entry.device || st.st_ino != entry.inode) {
                qWarning("External-core temporary file identity changed.");
                continue;
            }

            if (::unlinkat(directory_, encoded.constData(), 0) != 0 &&
                errno != ENOENT) {
                remaining.append(entry);
                qWarning("Cannot remove external-core temporary file.");
            }
#else
            const QString path = QDir(root_).filePath("temp/" + entry.name);
            if (QFileInfo::exists(path) && !QFile::remove(path)) {
                remaining.append(entry);
                qWarning("Cannot remove external-core temporary file.");
            }
#endif
        }
        entries_ = std::move(remaining);
    }

private:
    static bool validName(const QString &name) {
        if (name.isEmpty() || name == "." || name == "..")
            return false;
        for (const QChar c : name) {
            if (c.isNull() || c == '/' || c == '\\' || c == ':' ||
                c.unicode() < 0x20)
                return false;
        }
        return true;
    }

    static bool fail(QString &error, const char *stage) {
        error = QStringLiteral(
            "Cannot prepare external-core temporary file (%1).")
                    .arg(QString::fromLatin1(stage));
        return false;
    }

    bool prepareDirectory(QString &error) {
#ifdef Q_OS_UNIX
        struct DirectoryHandle {
            int fd;
            ~DirectoryHandle() {
                if (fd >= 0) ::close(fd);
            }
        };

        const QByteArray rootName = QFile::encodeName(root_);
        DirectoryHandle parent{::open(
            rootName.constData(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (parent.fd < 0)
            return fail(error, "parent directory");

        struct stat parentStat {};
        if (::fstat(parent.fd, &parentStat) != 0 ||
            !S_ISDIR(parentStat.st_mode) ||
            parentStat.st_uid != ::geteuid() ||
            (parentStat.st_mode & 0022) != 0) {
            return fail(error, "unsafe parent directory");
        }

        if (::mkdirat(parent.fd, "temp", 0700) != 0 && errno != EEXIST)
            return fail(error, "directory creation");

        DirectoryHandle current{::openat(
            parent.fd, "temp",
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (current.fd < 0)
            return fail(error, "temporary directory");

        struct stat st {};
        if (::fstat(current.fd, &st) != 0 ||
            !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
            return fail(error, "temporary directory ownership");
        }

        if (ops_.chmodFile(current.fd, 0700) != 0)
            return fail(error, "directory permissions");

        if (::fstat(current.fd, &st) != 0 ||
            !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
            (st.st_mode & 07777) != 0700) {
            return fail(error, "directory permission verification");
        }

        if (directory_ >= 0) {
            struct stat previous {};
            if (::fstat(directory_, &previous) != 0 ||
                previous.st_dev != st.st_dev ||
                previous.st_ino != st.st_ino) {
                return fail(error, "directory identity changed");
            }
        } else {
            directory_ = current.fd;
            current.fd = -1;
        }
        return true;
#else
        QDir root(root_);
        const QString path = root.filePath("temp");
        QFileInfo info(path);
        if (info.isSymLink())
            return fail(error, "temporary directory symlink");
        if (!info.exists()) {
            if (!root.mkdir("temp"))
                return fail(error, "directory creation");
            info.refresh();
        }
        if (!info.isDir() || info.isSymLink())
            return fail(error, "temporary directory");
        return true;
#endif
    }

    struct Entry {
        QString name;
#ifdef Q_OS_UNIX
        dev_t device;
        ino_t inode;
#endif
    };

    QString root_;
    QVector<Entry> entries_;
#ifdef Q_OS_UNIX
    TempFileOps ops_;
    int directory_ = -1;
#endif
};

// Call once, before start(), in the process's owning thread.
// Destruction cleanup occurs after QProcess's destructor handles its child.
inline void AttachExternalTempFiles(
    QProcess &process, std::shared_ptr<ExternalTempFiles> files) {
    if (!files)
        return;

    QObject::connect(
        &process,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        &process,
        [files](int, QProcess::ExitStatus) { files->clear(); });

    QObject::connect(
        &process, &QProcess::errorOccurred, &process,
        [files](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                files->clear();
        });

    QObject::connect(
        &process, &QObject::destroyed,
        [files](QObject *) { files->clear(); });
}

} // namespace NekoGui_sys
