#include <QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include "main/NekoGui.hpp"
#include "sys/ExternalProcess.hpp"
#include "sys/ExternalTempFiles.hpp"

#include <algorithm>
#include <memory>

namespace NekoGui {
DataStore *dataStore = nullptr;
}

void setTimeout(const std::function<void()> &callback,
                QObject *object, int timeout) {
    QTimer::singleShot(timeout, object, callback);
}

#ifdef Q_OS_UNIX
static int denyChmod(int, mode_t) {
    errno = EPERM;
    return -1;
}

static ssize_t zeroWrite(int, const void *, size_t) {
    return 0;
}

static ssize_t failWrite(int, const void *, size_t) {
    errno = ENOSPC;
    return -1;
}

static ssize_t shortWrite(int fd, const void *data, size_t count) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || (st.st_mode & 07777) != 0600) {
        errno = EACCES;
        return -1;
    }
    return ::write(fd, data, std::min<size_t>(count, 2));
}

static bool interruptedOnce = false;

static ssize_t interruptedWrite(int fd, const void *data, size_t count) {
    if (!interruptedOnce) {
        interruptedOnce = true;
        errno = EINTR;
        return -1;
    }
    return shortWrite(fd, data, count);
}

static int failingClose(int fd) {
    const int result = ::close(fd);
    if (result != 0)
        return result;
    errno = EIO;
    return -1;
}
#endif

class ExternalCredentialsTest : public QObject {
    Q_OBJECT

private:
    QStringList logs_;

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

private slots:
    void init() {
        logs_.clear();
        MW_show_log = [this](QString message) { logs_.append(message); };
        MW_show_log_ext = [this](QString tag, QString message) {
            logs_.append(tag + ":" + message);
        };
        MW_show_log_ext_vt100 =
            [this](QString message) { logs_.append(message); };
        MW_dialog_message = [](QString, QString) {};
    }

    void cleanup() {
        MW_show_log = {};
        MW_show_log_ext = {};
        MW_show_log_ext_vt100 = {};
        MW_dialog_message = {};
    }

    void payloadAndCleanup() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QString path;
        QString error;
        const QByteArray payload("password=PR8_FILE_SECRET\n");

        {
            NekoGui_sys::ExternalTempFiles files(root.path());
            QVERIFY2(files.write("test.json", payload, path, error),
                     qPrintable(error));
            QVERIFY(QFileInfo(path).isAbsolute());
            QCOMPARE(readFile(path), payload);

#ifdef Q_OS_UNIX
            struct stat directory {};
            struct stat file {};
            const QByteArray dirName =
                QFile::encodeName(root.path() + "/temp");
            const QByteArray fileName = QFile::encodeName(path);
            QCOMPARE(::stat(dirName.constData(), &directory), 0);
            QCOMPARE(::stat(fileName.constData(), &file), 0);
            QCOMPARE(int(directory.st_mode & 07777), 0700);
            QCOMPARE(int(file.st_mode & 07777), 0600);
            QVERIFY(directory.st_uid == ::geteuid());
            QVERIFY(file.st_uid == ::geteuid());
#endif
        }
        QVERIFY(!QFileInfo::exists(path));
    }

    void invalidNames() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NekoGui_sys::ExternalTempFiles files(root.path());
        const QStringList names{
            "", ".", "..", "../escape", "x/y", "x\\y", "x:y",
            QString("x") + QChar(0) + "y"
        };
        for (const QString &name : names) {
            QString path = "must-clear";
            QString error;
            QVERIFY(!files.write(name, "secret", path, error));
            QVERIFY(path.isEmpty());
            QVERIFY(!error.isEmpty());
        }
        QVERIFY(!QFileInfo::exists(root.path() + "/temp"));
    }

    void existingFileIsNotTruncated() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NekoGui_sys::ExternalTempFiles first(root.path());
        NekoGui_sys::ExternalTempFiles second(root.path());
        QString path;
        QString error;
        QVERIFY(first.write("same.json", "original", path, error));

        QString rejected;
        QVERIFY(!second.write("same.json", "replacement", rejected, error));
        QVERIFY(rejected.isEmpty());
        QCOMPARE(readFile(path), QByteArray("original"));
        second.clear();
        QCOMPARE(readFile(path), QByteArray("original"));
    }

    void independentOwners() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        NekoGui_sys::ExternalTempFiles first(root.path());
        NekoGui_sys::ExternalTempFiles second(root.path());
        QString a;
        QString b;
        QString error;
        QVERIFY(first.write("a.json", "a", a, error));
        QVERIFY(second.write("b.json", "b", b, error));
        first.clear();
        first.clear();
        QVERIFY(!QFileInfo::exists(a));
        QCOMPARE(readFile(b), QByteArray("b"));
        second.clear();
        QVERIFY(!QFileInfo::exists(b));
    }

    void launchLogDoesNotExposeInputs() {
        NekoGui_sys::ExternalProcess process;
        process.tag = "test";
        process.program = QCoreApplication::applicationFilePath();
        process.arguments = QStringList{
            "--pr8-exit", "--proxy=https://PR8_USER:PR8_PASSWORD@invalid",
            "--password", "PR8_SPLIT_PASSWORD",
            "--unknown-secret=PR8_CUSTOM_SECRET"
        };

        QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();
        environment.insert("PR8_ENV_SECRET", "PR8_ENV_VALUE");
        process.env = environment.toStringList();

        process.Start();
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);

        const QString combined = logs_.join("\n");
        QVERIFY(combined.contains("External core starting"));
        QVERIFY(!combined.contains(process.program));
        for (const QString &secret : QStringList{
                 "PR8_USER", "PR8_PASSWORD", "PR8_SPLIT_PASSWORD",
                 "PR8_CUSTOM_SECRET", "PR8_ENV_SECRET", "PR8_ENV_VALUE"}) {
            QVERIFY2(!combined.contains(secret), qPrintable(secret));
        }
    }

    void processLifecycle_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("normal-exit") << 0;
        QTest::newRow("kill") << 1;
        QTest::newRow("failed-start") << 2;
        QTest::newRow("destroy-unstarted") << 3;
        QTest::newRow("crash") << 4;
    }

    void processLifecycle() {
        QFETCH(int, mode);
        QTemporaryDir root;
        QVERIFY(root.isValid());

        auto files =
            std::make_shared<NekoGui_sys::ExternalTempFiles>(root.path());
        QString path;
        QString error;
        QVERIFY(files->write("owned.json", "secret", path, error));

        auto process = std::make_unique<NekoGui_sys::ExternalProcess>();
        process->managed = false;
        process->program = QCoreApplication::applicationFilePath();
        process->arguments =
            QStringList{mode == 0 ? "--pr8-exit" : "--pr8-wait"};
        NekoGui_sys::AttachExternalTempFiles(*process, files);

        if (mode == 3) {
            process.reset();
        } else if (mode == 2) {
            process->program = root.path() + "/missing-program";
            process->Start();
            QVERIFY(!process->waitForStarted(5000));
        } else {
            process->Start();
            QVERIFY(process->waitForStarted(5000));
            if (mode == 1)
                process->Kill();
            if (mode == 4)
                process->QProcess::kill();
            if (process->state() != QProcess::NotRunning)
                QVERIFY(process->waitForFinished(5000));
        }

        QTRY_VERIFY(!QFileInfo::exists(path));
        // Keep a build-result-like shared reference alive throughout:
        QVERIFY(files != nullptr);
    }

    void posixDirectoryAndSymlinkChecks() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX ownership and mode test");
#else
        QTemporaryDir root;
        QTemporaryDir target;
        QVERIFY(root.isValid());
        QVERIFY(target.isValid());

        const QString temp = root.path() + "/temp";
        const QByteArray tempName = QFile::encodeName(temp);
        QVERIFY(QDir(root.path()).mkdir("temp"));
        QCOMPARE(::chmod(tempName.constData(), 0777), 0);

        NekoGui_sys::ExternalTempFiles files(root.path());
        QString path;
        QString error;
        QVERIFY(files.write("first.json", "secret", path, error));

        struct stat st {};
        QCOMPARE(::stat(tempName.constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 07777), 0700);
        files.clear();

        const QString targetFile = target.path() + "/target";
        QFile targetOutput(targetFile);
        QVERIFY(targetOutput.open(QIODevice::WriteOnly));
        QCOMPARE(targetOutput.write("untouched"), qint64(9));
        targetOutput.close();

        const QByteArray linkName =
            QFile::encodeName(temp + "/collision.json");
        const QByteArray targetName = QFile::encodeName(targetFile);
        QCOMPARE(::symlink(targetName.constData(), linkName.constData()), 0);
        QVERIFY(!files.write("collision.json", "secret", path, error));
        QCOMPARE(readFile(targetFile), QByteArray("untouched"));
        QCOMPARE(::unlink(linkName.constData()), 0);

        QTemporaryDir symlinkRoot;
        QVERIFY(symlinkRoot.isValid());
        const QByteArray directoryLink =
            QFile::encodeName(symlinkRoot.path() + "/temp");
        const QByteArray directoryTarget = QFile::encodeName(target.path());
        QCOMPARE(::symlink(directoryTarget.constData(),
                           directoryLink.constData()), 0);
        NekoGui_sys::ExternalTempFiles rejected(symlinkRoot.path());
        QVERIFY(!rejected.write("rejected.json", "secret", path, error));
        QVERIFY(!QFileInfo::exists(target.path() + "/rejected.json"));

        QTemporaryDir regularRoot;
        QVERIFY(regularRoot.isValid());
        QFile regular(regularRoot.path() + "/temp");
        QVERIFY(regular.open(QIODevice::WriteOnly));
        regular.close();
        NekoGui_sys::ExternalTempFiles notDirectory(regularRoot.path());
        QVERIFY(!notDirectory.write("x.json", "secret", path, error));
#endif
    }

    void posixFailuresAndShortWrites() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX fault injection");
#else
        for (int failure = 0; failure < 4; ++failure) {
            QTemporaryDir root;
            QVERIFY(root.isValid());
            NekoGui_sys::TempFileOps ops;
            if (failure == 0) ops.chmodFile = denyChmod;
            if (failure == 1) ops.writeFile = zeroWrite;
            if (failure == 2) ops.writeFile = failWrite;
            if (failure == 3) ops.closeFile = failingClose;

            NekoGui_sys::ExternalTempFiles files(root.path(), ops);
            QString path = "must-clear";
            QString error;
            QVERIFY(!files.write("failed.json", "secret", path, error));
            QVERIFY(path.isEmpty());
            QVERIFY(!error.isEmpty());
            QVERIFY(!QFileInfo::exists(root.path() + "/temp/failed.json"));
        }

        for (int mode = 0; mode < 2; ++mode) {
            QTemporaryDir root;
            QVERIFY(root.isValid());
            NekoGui_sys::TempFileOps ops;
            interruptedOnce = false;
            ops.writeFile = mode == 0 ? shortWrite : interruptedWrite;
            NekoGui_sys::ExternalTempFiles files(root.path(), ops);
            QString path;
            QString error;
            const QByteArray data("partial-write-and-eintr");
            QVERIFY2(files.write("complete.json", data, path, error),
                     qPrintable(error));
            QCOMPARE(readFile(path), data);
        }

        QTemporaryDir unsafeParent;
        QVERIFY(unsafeParent.isValid());
        const QByteArray parent = QFile::encodeName(unsafeParent.path());
        QCOMPARE(::chmod(parent.constData(), 0777), 0);
        NekoGui_sys::ExternalTempFiles rejected(unsafeParent.path());
        QString path;
        QString error;
        QVERIFY(!rejected.write("x.json", "secret", path, error));
        QVERIFY(!QFileInfo::exists(unsafeParent.path() + "/temp"));
        QCOMPARE(::chmod(parent.constData(), 0700), 0);
#endif
    }

    void foreignOwnedDirectory() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX ownership test");
#else
        const QString root =
            QString::fromLocal8Bit(qgetenv("PR8_FOREIGN_ROOT"));
        if (root.isEmpty())
            QSKIP("Run the owner-created foreign-directory fixture in §4.3");

        struct stat st {};
        const QByteArray path = QFile::encodeName(root + "/temp");
        QCOMPARE(::lstat(path.constData(), &st), 0);
        QVERIFY(st.st_uid != ::geteuid());

        NekoGui_sys::ExternalTempFiles files(root);
        QString output;
        QString error;
        QVERIFY(!files.write("must-not-exist.json", "secret", output, error));
        QVERIFY(output.isEmpty());
        QVERIFY(!QFileInfo::exists(root + "/temp/must-not-exist.json"));
#endif
    }
};

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();

    if (arguments.contains("--pr8-exit"))
        return 0;
    if (arguments.contains("--pr8-wait")) {
        QTimer::singleShot(30000, &application, &QCoreApplication::quit);
        return application.exec();
    }

    ExternalCredentialsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_external_credentials.moc"
