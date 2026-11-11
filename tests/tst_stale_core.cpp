#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include "sys/StaleCore.hpp"
#include "sys/LinuxOwner.hpp"
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace NekoGui_sys::StaleCore;
Q_DECLARE_METATYPE(OwnerState)
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
    void linuxClockBase_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<OwnerState>("state");
        QTest::newRow("different-namespace-mismatched-start") << QString("different") << OwnerState::UNKNOWN;
        QTest::newRow("different-namespace-matching-start") << QString("different-matching") << OwnerState::UNKNOWN;
        QTest::newRow("same-namespace-matching-start") << QString("matching") << OwnerState::MATCHING_LIVE;
        QTest::newRow("same-namespace-mismatched-start") << QString("reused") << OwnerState::CONFIRMED_REUSED;
        QTest::newRow("unreadable-owner-namespace") << QString("owner-unreadable") << OwnerState::UNKNOWN;
        QTest::newRow("unreadable-reader-namespace") << QString("reader-unreadable") << OwnerState::UNKNOWN;
        QTest::newRow("unreadable-boot-id") << QString("boot-unreadable") << OwnerState::UNKNOWN;
        QTest::newRow("different-recording-boot") << QString("other-boot") << OwnerState::UNKNOWN;
        QTest::newRow("different-recording-namespace") << QString("other-recording-ns") << OwnerState::UNKNOWN;
        QTest::newRow("legacy-record-no-clock-base") << QString("legacy") << OwnerState::UNKNOWN;
        QTest::newRow("namespace-changes-during-read") << QString("changing") << OwnerState::UNKNOWN;
        QTest::newRow("malformed-stat") << QString("malformed") << OwnerState::UNKNOWN;
    }
    void linuxClockBase() {
        QFETCH(QString, kind);
        QFETCH(OwnerState, state);
        const QByteArray boot("12345678-1234-1234-1234-123456789abc");
        const QString ours("/proc/thread-self/ns/time"), theirs("/proc/456/ns/time");
        const QString stat("/proc/456/stat"), bootPath("/proc/sys/kernel/random/boot_id");
        QHash<QString, QByteArray> proc{{ours, "time:[100]"}, {theirs, "time:[100]"}, {bootPath, boot + '\n'}};
        // Field 22, after a comm containing spaces and parentheses.
        proc[stat] = "456 (live (gui)) S";
        for (int field = 4; field < 22; ++field) proc[stat] += " 0";
        proc[stat] += " 100\n";
        bool statRead = false, changesEnabled = false;
        LinuxOwner::Reader reader = [&](const QString &path) -> LinuxOwner::ReadResult {
            if (changesEnabled && kind == "changing" && statRead && path == ours) return {"time:[200]"};
            if (path == stat) statRead = true;
            return {proc.value(path)};
        };
        auto owner = LinuxOwner::sample(456, reader).identity;
        QCOMPARE(owner.start, QString("100"));
        QCOMPARE(owner.clockBase, QString::fromLatin1(boot + "/time:[100]"));
        statRead = false;
        changesEnabled = true;
        if (kind != "matching" && kind != "different-matching") owner.start = "99";
        if (kind.startsWith("different")) proc[theirs] = "time:[200]";
        if (kind == "owner-unreadable") proc.remove(theirs);
        if (kind == "reader-unreadable") proc.remove(ours);
        if (kind == "boot-unreadable") proc.remove(bootPath);
        if (kind == "other-boot") owner.clockBase.replace("12345678-", "87654321-");
        if (kind == "other-recording-ns") owner.clockBase.replace("[100]", "[200]");
        if (kind == "legacy") owner.clockBase.clear();
        if (kind == "malformed") proc[stat].chop(1);
        QCOMPARE(LinuxOwner::observeOwner(owner, reader), state);
        // Failed clock-base observation must retain owner metadata so that the
        // record cannot fall through to legacy authenticated Exit recovery.
        if (kind.endsWith("unreadable") || kind.startsWith("different")) {
            const auto sampled = LinuxOwner::sample(456, reader).identity;
            QCOMPARE(sampled.pid, qint64(456));
            QCOMPARE(sampled.start, QString("100"));
            QVERIFY(sampled.clockBase.isEmpty());
        }
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path());
        value.owner = owner;
        QVERIFY(write(path, value));
        Record loaded;
        QVERIFY(read(path, loaded));
        QCOMPARE(loaded.owner.clockBase, owner.clockBase);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        int requests = 0, listenerChecks = 0;
        Operations operations;
        operations.ownerState = [&](const Identity &expected) {
            statRead = false;
            return LinuxOwner::observeOwner(expected, reader);
        };
        operations.observeProcess = [](qint64) { return Identity{}; };
        operations.exit = [&](const Record &) { ++requests; };
        operations.listener = [&](int) { ++listenerChecks; return 0; };
        std::atomic_bool cancelled{false};
        QString refusal;
        const bool stale = state == OwnerState::CONFIRMED_REUSED;
        QCOMPARE(wait(path, cancelled, operations, 40, &refusal), stale);
        QCOMPARE(requests, stale ? 1 : 0);
        QCOMPARE(listenerChecks, stale ? 1 : 0);
        QCOMPARE(QFile::exists(path), !stale);
        if (!stale) {
            QVERIFY(!refusal.isEmpty());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), original);
        }
    }
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
    void guiOwnerIdentity_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("refuse");
        QTest::newRow("live-sibling") << QString("live") << true;
        QTest::newRow("unknown-owner") << QString("unknown") << true;
        QTest::newRow("dead-owner") << QString("dead") << false;
        QTest::newRow("reused-pid") << QString("reused") << false;
        QTest::newRow("this-gui") << QString("self") << false;
        QTest::newRow("legacy-no-owner") << QString("legacy") << false;
        QTest::newRow("incomplete-owner") << QString("incomplete") << false;
    }
    void guiOwnerIdentity() {
        QFETCH(QString, kind);
        QFETCH(bool, refuse);
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path());
        value.owner = {kind == "self" ? QCoreApplication::applicationPid() : 456, "gui-start", {}};
        QVERIFY(write(path, value));
        if (kind == "legacy" || kind == "incomplete") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            auto object = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            if (kind == "legacy") object.remove("owner");
            else object["owner"] = QJsonObject{{"pid", "456"}};
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(object).toJson());
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        Record loaded;
        QVERIFY(read(path, loaded));
        if (kind != "legacy" && kind != "incomplete") {
            QCOMPARE(loaded.owner.pid, value.owner.pid);
            QCOMPARE(loaded.owner.start, value.owner.start);
        }
        int requests = 0;
        bool ownerObserved = false;
        Operations operations;
        operations.ownerState = [&](const Identity &expected) {
            ownerObserved = expected.pid == value.owner.pid && expected.start == value.owner.start;
            if (kind == "unknown") return OwnerState::UNKNOWN;
            if (kind == "dead") return OwnerState::CONFIRMED_DEAD;
            if (kind == "reused") return OwnerState::CONFIRMED_REUSED;
            return OwnerState::MATCHING_LIVE;
        };
        operations.observeProcess = [](qint64) { return Identity{}; };
        operations.exit = [&](const Record &) { ++requests; };
        operations.listener = [](int) { return 0; };
        std::atomic_bool cancelled{false};
        QString refusal;
        QCOMPARE(wait(path, cancelled, operations, 40, &refusal), !refuse);
        QCOMPARE(ownerObserved, kind != "self" && kind != "legacy" && kind != "incomplete");
        QCOMPARE(requests, refuse ? 0 : 1);
        QCOMPARE(QFile::exists(path), refuse);
        if (refuse) {
            QVERIFY(refusal.contains(kind == "unknown" ? "Cannot determine whether another NekoBox GUI" :
                                                       "Another NekoBox GUI is using this configuration"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), original);
        } else QVERIFY(refusal.isEmpty());
    }
    void unobservableLiveOwner() {
        QProcess owner;
        owner.start(QStringLiteral("/bin/sleep"), {QStringLiteral("10")});
        QVERIFY(owner.waitForStarted());
        QTemporaryDir root;
        const auto path = root.filePath(".core-launch.json");
        auto value = record(root.path());
        value.owner = observe(owner.processId());
        QVERIFY(value.owner.pid > 0);
        QCOMPARE(observeOwner(value.owner), OwnerState::MATCHING_LIVE);
        // Owner observation does not depend on executable identity.
        value.owner.executable.clear();
        QCOMPARE(observeOwner(value.owner), OwnerState::MATCHING_LIVE);
        auto reused = value.owner;
        reused.start += "-reused";
        QCOMPARE(observeOwner(reused), OwnerState::CONFIRMED_REUSED);
        QVERIFY(write(path, value));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        int observations = 0, requests = 0, listenerChecks = 0;
        Operations operations;
        operations.ownerState = [&](const Identity &expected) {
            ++observations;
            return expected.pid == value.owner.pid ? OwnerState::UNKNOWN : OwnerState::CONFIRMED_DEAD;
        };
        operations.exit = [&](const Record &) { ++requests; };
        operations.listener = [&](int) { ++listenerChecks; return 0; };
        std::atomic_bool cancelled{false};
        QString refusal;
        QVERIFY(!wait(path, cancelled, operations, 40, &refusal));
        QCOMPARE(observations, 1);
        QCOMPARE(requests, 0);
        QCOMPARE(listenerChecks, 0);
        QVERIFY(refusal.contains("Cannot determine whether another NekoBox GUI"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        QCOMPARE(owner.state(), QProcess::Running);
        owner.terminate(); // Only this test's controlled child; not record-based signaling.
        QVERIFY(owner.waitForFinished());
        QCOMPARE(observeOwner(value.owner), OwnerState::CONFIRMED_DEAD);
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
        start(&context, root.filePath("no-record"), [&](bool ready, const QString &) {
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
