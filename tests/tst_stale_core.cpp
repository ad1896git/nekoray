#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include "sys/StaleCore.hpp"
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace NekoGui_sys::StaleCore;
class StaleCoreTest : public QObject {
    Q_OBJECT
    static Record record(const QString &directory) {
        Record result;
        result.token = QByteArray(64, 'a');
        result.port = 19810;
        result.config = directory;
        result.process = {123, "original-start", "/owned/core"};
        return result;
    }
private slots:
    void staleActionsHaveNoSignalApi() {
        QFile source(QString::fromUtf8(NKR_STALE_SOURCE));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto text = QString::fromUtf8(source.readAll());
        const QRegularExpression signalApi(QStringLiteral("\\b(kill|killpg|proc_signal[a-zA-Z_]*|proc_terminate[a-zA-Z_]*)\\s*\\("));
        QVERIFY(!signalApi.match(text).hasMatch());
    }
    void privateRecordAndCleanStop() {
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        const auto value = record(root.path());
        QVERIFY(write(path, value));
        Record loaded;
        QVERIFY(read(path, loaded));
        QCOMPARE(loaded.token, value.token);
#ifdef Q_OS_UNIX
        struct stat st{};
        QVERIFY(::stat(QFile::encodeName(path).constData(), &st) == 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QCOMPARE(st.st_uid, ::geteuid());
#endif
        removeOwned(path, QByteArray(64, 'b'));
        QVERIFY(QFile::exists(path));
        removeOwned(path, value.token);
        QVERIFY(!QFile::exists(path));
    }
    void invalidRecord() {
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        for (const auto &bytes : {QByteArray("bad json"), QByteArray(4097, 'x')}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes); file.close();
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            std::atomic_bool cancelled{false};
            QVERIFY(wait(path, cancelled));
            QVERIFY(!QFile::exists(path));
        }
#ifdef Q_OS_UNIX
        QVERIFY(write(path, record(root.path())));
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 0644) == 0);
        Record loaded;
        QVERIFY(!read(path, loaded));
        QFile::remove(path);
        // A symlink to a foreign-owned file must never be followed or altered.
        QVERIFY(::symlink("/etc/passwd", QFile::encodeName(path).constData()) == 0);
        QVERIFY(!read(path, loaded));
        std::atomic_bool cancelled{false};
        QVERIFY(wait(path, cancelled));
        QVERIFY(QFile::exists("/etc/passwd"));
#endif
    }
    void malformedToken() {
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path());
        value.token += '\n';
        QVERIFY(write(path, value));
        Record loaded;
        QVERIFY(!read(path, loaded));
    }
    void foreignOwner() {
#ifdef Q_OS_UNIX
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        QVERIFY(write(path, record(root.path())));
        if (::chown(QFile::encodeName(path).constData(), ::geteuid() + 1, -1) != 0)
            QSKIP("Cannot create foreign-owned regular record without elevation; symlink and mode rejection are covered");
        Record value;
        QVERIFY(!read(path, value));
#endif
    }
    void reusedPidAndUnrelatedProcess() {
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path());
#ifdef Q_OS_UNIX
        value.process = observe(::getpid()); // this test executable is unrelated
        QVERIFY(value.process.pid != 0);
        value.process.start += "-reused";
#endif
        QVERIFY(write(path, value));
        int authenticatedRequests = 0;
        Operations operations;
        operations.exit = [&](const Record &request) {
            QCOMPARE(request.token, value.token);
            ++authenticatedRequests;
        };
        operations.listener = [](int) { return 0; };
        std::atomic_bool cancelled{false};
        QVERIFY(wait(path, cancelled, operations, 40));
        QCOMPARE(authenticatedRequests, 1);
        QVERIFY(!QFile::exists(path));
        // StaleCore's operation surface has no signal API. The still-running
        // test executable above was the unrelated/reused-PID target.
    }
    void selfExitAndRefusal() {
        for (bool exits : {true, false}) {
            QTemporaryDir root;
            const auto path = root.filePath(".core-launch.json");
            const auto value = record(root.path());
            QVERIFY(write(path, value));
            QElapsedTimer elapsed; elapsed.start();
            Operations operations;
            operations.exit = [](const Record &) {}; // unavailable/ignored Exit
            operations.observeProcess = [&](qint64) {
                return exits && elapsed.elapsed() >= 30 ? Identity{} : value.process;
            };
            operations.listener = [&](int) { return exits && elapsed.elapsed() >= 30 ? 0 : 1; };
            std::atomic_bool cancelled{false};
            QCOMPARE(wait(path, cancelled, operations, 80), exits);
            QCOMPARE(QFile::exists(path), !exits);
        }
    }
    void beforeSpawnReservation() {
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path()); value.process = {};
        QVERIFY(write(path, value));
        Operations operations;
        operations.exit = [](const Record &) {};
        operations.listener = [](int) { return 0; };
        std::atomic_bool cancelled{false};
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(wait(path, cancelled, operations, 80));
        QVERIFY(elapsed.elapsed() >= 80);
        QVERIFY(!QFile::exists(path));
    }
    void asynchronousCompletionAndContextLifetime() {
        QTemporaryDir root;
        int regular = 0, tun = 0;
        QObject context;
        start(&context, root.filePath("no-record"), [&](bool ready) {
            QVERIFY(ready);
            QCOMPARE(QThread::currentThread(), qApp->thread());
            ++regular; ++tun;
        });
        QCOMPARE(regular, 0); QCOMPARE(tun, 0);
        QTRY_COMPARE(regular, 1); QCOMPARE(tun, 1);
    }
};
QTEST_GUILESS_MAIN(StaleCoreTest)
#include "tst_stale_core.moc"
