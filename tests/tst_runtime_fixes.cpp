#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <stdexcept>

#include "3rdparty/QThreadCreateThread.hpp"
#include "db/traffic/TrafficBatch.hpp"
#include "db/traffic/TrafficSampleClock.hpp"
#include "sys/VpnProcess.hpp"

using NekoGui_runtime::TunInterfaceName;
using NekoGui_runtime::VpnProcess;
using namespace NekoGui_traffic;

namespace {
struct Counters {
    long long uplink = 100;
    long long downlink = 200;
    long long uplink_rate = 999;
    long long downlink_rate = 999;
};

QStringList ChildArguments(const QString &releaseFile, const QString &action) {
    return {QStringLiteral("--vpn-child"), releaseFile, action};
}

bool ReleaseChild(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}
} // namespace

class RuntimeFixesTest : public QObject {
    Q_OBJECT

private slots:
    void firstSampleAndRestartPreserveTotals() {
        TrafficSampleClock clock;
        const TrafficSampleClock::TimePoint zero{};
        Counters counters;

        // TimePoint{} is a valid timestamp, not an initialization sentinel.
        const auto first = clock.Sample(zero);
        QVERIFY(!first);
        ApplyTrafficDelta(counters, TrafficDelta{10, 20}, first);
        QCOMPARE(counters.uplink, 110LL);
        QCOMPARE(counters.downlink, 220LL);
        QCOMPARE(counters.uplink_rate, 0LL);
        QCOMPARE(counters.downlink_rate, 0LL);

        const auto second = clock.Sample(zero + std::chrono::milliseconds(500));
        QVERIFY(second);
        QCOMPARE(*second, 500LL);
        ApplyTrafficDelta(counters, TrafficDelta{15, 25}, second);
        QCOMPARE(counters.uplink, 125LL);
        QCOMPARE(counters.downlink, 245LL);
        QCOMPARE(counters.uplink_rate, 30LL);
        QCOMPARE(counters.downlink_rate, 50LL);

        clock.Reset();
        const auto restarted = clock.Sample(zero + std::chrono::hours(1));
        QVERIFY(!restarted);
        ApplyTrafficDelta(counters, TrafficDelta{7, 9}, restarted);
        QCOMPARE(counters.uplink, 132LL);
        QCOMPARE(counters.downlink, 254LL);
        QCOMPARE(counters.uplink_rate, 0LL);
        QCOMPARE(counters.downlink_rate, 0LL);
    }

    void firstFinalSampleStillAddsBytes() {
        TrafficSampleClock clock;
        Counters counters;
        ApplyTrafficDelta(counters, TrafficDelta{3, 4},
                          clock.Sample(TrafficSampleClock::TimePoint{}));
        QCOMPARE(counters.uplink, 103LL);
        QCOMPARE(counters.downlink, 204LL);
        QCOMPARE(counters.uplink_rate, 0LL);
        QCOMPARE(counters.downlink_rate, 0LL);
    }

    void knownZeroIntervalKeepsExistingBehavior() {
        Counters counters;
        ApplyTrafficDelta(counters, TrafficDelta{2, 3}, 0);
        QCOMPARE(counters.uplink_rate, 2000LL);
        QCOMPARE(counters.downlink_rate, 3000LL);
    }

    void workerException_data() {
        QTest::addColumn<bool>("standard");
        QTest::newRow("std-exception") << true;
        QTest::newRow("unknown-exception") << false;
    }

    void workerException() {
        QFETCH(bool, standard);
        QTest::ignoreMessage(QtWarningMsg, standard
            ? "Background task failed: injected"
            : "Background task failed: unknown exception");
        QPointer<QThread> thread = createQThread([standard] {
            if (standard) throw std::runtime_error("injected");
            throw 42;
        });
        QSignalSpy finished(thread.data(), &QThread::finished);
        thread->start();
        QVERIFY(thread->wait(5000));
        QTRY_COMPARE(finished.count(), 1);
        QTRY_VERIFY(thread.isNull());

        std::atomic<bool> ran{false};
        QPointer<QThread> next = createQThread([&ran] { ran.store(true); });
        next->start();
        QVERIFY(next->wait(5000));
        QVERIFY(ran.load());
        QTRY_VERIFY(next.isNull());
    }

    void configuredInterfaceName() {
        const QByteArray config = "\n"
            "        {\n"
            "            // Generated templates contain comments.\n"
            "            \"note\": \"https://example.invalid/a/*b*/\",\n"
            "            \"inbounds\": [\n"
            "                {\n"
            "                    \"type\": \"tun\",\n"
            "                    /* A custom template can choose its own name. */\n"
            "                    \"interface_name\": \"custom-tun\",\n"
            "                },\n"
            "            ],\n"
            "        }\n"
            "        ";
        QCOMPARE(TunInterfaceName(config), QStringLiteral("custom-tun"));
        QCOMPARE(TunInterfaceName(
            "{\"inbounds\":[{\"type\":\"tun\",\"interface_name\":\"utun9\"}]}"),
            QStringLiteral("utun9"));
        QVERIFY(TunInterfaceName("{}").isEmpty());
        QVERIFY(TunInterfaceName("{broken").isEmpty());
        QVERIFY(TunInterfaceName("/* unfinished").isEmpty());
        QVERIFY(TunInterfaceName("{\"inbounds\":[{\"type\":\"tun\"}]}").isEmpty());
        QVERIFY(TunInterfaceName("{\"inbounds\":[\n"
            "            {\"type\":\"tun\",\"interface_name\":\"a\"},\n"
            "            {\"type\":\"tun\",\"interface_name\":\"b\"}\n"
            "        ]}").isEmpty());
    }

    void missingIntermediaryFailsSynchronously() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        VpnProcess process([] { return false; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(!process.start(dir.filePath("missing-vpn-launcher"), {}));
        QVERIFY(process.errorString().contains(QStringLiteral("Failed to start VPN launcher")));
        QCOMPARE(process.processId(), qint64(0));
        QVERIFY(!process.isReady());
        QCoreApplication::processEvents();
        QCOMPARE(ended.count(), 0);
        QCOMPARE(ready.count(), 0);
    }

    void existingInterfaceCannotProveNewReadiness() {
        VpnProcess process([] { return true; });
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(!process.start(QCoreApplication::applicationFilePath(), {}));
        QVERIFY(process.errorString().contains(QStringLiteral("already exists")));
        QCOMPARE(process.processId(), qint64(0));
        QCOMPARE(ready.count(), 0);
    }

    void earlyExit_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("zero") << QStringLiteral("0");
        QTest::newRow("126") << QStringLiteral("126");
        QTest::newRow("127") << QStringLiteral("127");
        QTest::newRow("crash") << QStringLiteral("crash");
    }

    void earlyExit() {
        QFETCH(QString, action);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto release = dir.filePath("release");
        VpnProcess process([] { return false; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(process.start(QCoreApplication::applicationFilePath(), ChildArguments(release, action)));
        QVERIFY(!process.isReady());
        QCOMPARE(ended.count(), 0);
        QVERIFY(ReleaseChild(release));
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ready.count(), 0);
        const auto error = ended.at(0).at(0).toString();
        QVERIFY(error.contains(QStringLiteral("before Tun became ready")));
        QVERIFY(error.contains(QStringLiteral("exit code")));
        if (action == QStringLiteral("crash"))
            QVERIFY(error.contains(QStringLiteral("crashed")));
        else
            QVERIFY(error.contains(QStringLiteral("exit code %1").arg(action)));
        QCoreApplication::processEvents();
        QCOMPARE(ended.count(), 1);
    }

    void exitAfterReadiness_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("clean") << QStringLiteral("0");
        QTest::newRow("nonzero") << QStringLiteral("7");
        QTest::newRow("crash") << QStringLiteral("crash");
    }

    void exitAfterReadiness() {
        QFETCH(QString, action);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto release = dir.filePath("release");
        bool interfacePresent = false;
        VpnProcess process([&] { return interfacePresent; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(process.start(QCoreApplication::applicationFilePath(), ChildArguments(release, action)));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!process.isReady());
        interfacePresent = true;
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(process.isReady());
        QCOMPARE(ended.count(), 0);
        QVERIFY(ReleaseChild(release));
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(ready.count(), 1);
        const auto error = ended.at(0).at(0).toString();
        if (action == QStringLiteral("0")) {
            QVERIFY(error.isEmpty());
        } else {
            QVERIFY(error.contains(QStringLiteral("after Tun became ready")));
            if (action == QStringLiteral("crash"))
                QVERIFY(error.contains(QStringLiteral("crashed")));
            else
                QVERIFY(error.contains(QStringLiteral("exit code 7")));
        }
    }

    void expectedStopDoesNotReportFailure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto release = dir.filePath("release");
        bool interfacePresent = false;
        VpnProcess process([&] { return interfacePresent; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(process.start(QCoreApplication::applicationFilePath(),
                              ChildArguments(release, QStringLiteral("7"))));
        process.setStopRequested(true);
        interfacePresent = true;
        QVERIFY(ReleaseChild(release));
        QTRY_COMPARE(ended.count(), 1);
        QVERIFY(ended.at(0).at(0).toString().isEmpty());
        QCOMPARE(ready.count(), 0);
    }

    void failedStopRestoresObservation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto release = dir.filePath("release");
        bool interfacePresent = false;
        VpnProcess process([&] { return interfacePresent; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QSignalSpy ready(&process, &VpnProcess::ready);
        QVERIFY(process.start(QCoreApplication::applicationFilePath(),
                              ChildArguments(release, QStringLiteral("7"))));
        process.setStopRequested(true);
        process.setStopRequested(false);
        interfacePresent = true;
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(ReleaseChild(release));
        QTRY_COMPARE(ended.count(), 1);
        QVERIFY(ended.at(0).at(0).toString().contains(QStringLiteral("after Tun became ready")));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--vpn-child")) {
        const auto release = arguments.at(2);
        const auto action = arguments.at(3);
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &app, [&] {
            if (!QFileInfo::exists(release)) return;
            if (action == QStringLiteral("crash")) std::abort();
            app.exit(action.toInt());
        });
        poll.start(10);
        QTimer::singleShot(10000, &app, [&] { app.exit(98); });
        return app.exec();
    }
    RuntimeFixesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_runtime_fixes.moc"
