#include <QtTest>
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include "main/UiOwnership.hpp"
#include "main/UiSnapshot.hpp"
#include "db/traffic/TrafficBatch.hpp"
using namespace NekoGui_concurrency;
using namespace NekoGui_traffic;
namespace {
struct Entity { int saves = 0; int value = 0; };
struct Counters {
    long long uplink = 0, downlink = 0, uplink_rate = 0, downlink_rate = 0;
};
struct ConfigBean {
    QJsonObject config{{"name", "original"}, {"server", "old.example"},
                       {"port", 443}, {"tls", QJsonObject{{"sni", "old.example"}}}};
    QJsonObject ToJson() const { return config; }
};
struct ConfigProfile {
    QString type = QStringLiteral("trojan");
    std::shared_ptr<ConfigBean> bean = std::make_shared<ConfigBean>();
    Counters traffic;
    int latency = 0;
    QString report;
    int saves = 0;
};
}
class W03bTest : public QObject {
    Q_OBJECT
private slots:
    void completionRunsOnUiExactlyOnce() {
        int calls = 0;
        QThread *callbackThread = nullptr;
        std::atomic<bool> posted{false};
        std::thread worker([&] {
            posted.store(PostUi([&] { ++calls; callbackThread = QThread::currentThread(); }));
        });
        worker.join();
        QVERIFY(posted.load());
        QCOMPARE(calls, 0);
        QTRY_COMPARE(calls, 1);
        QCOMPARE(callbackThread, QCoreApplication::instance()->thread());
        QCoreApplication::processEvents();
        QCOMPARE(calls, 1);
    }
    void uiPostingIsAlwaysQueued() {
        bool called = false;
        QVERIFY(PostUi([&] { called = true; }));
        QVERIFY(!called);
        QTRY_VERIFY(called);
    }
    void queuedCompletionDoesNotUseWorkerStack() {
        int received = 0;
        std::thread worker([&] {
            const int workerLocal = 73;
            PostUi([workerLocal, &received] { received = workerLocal; });
        });
        worker.join();
        QTRY_COMPARE(received, 73);
    }
    void deletedObjectIsNotSaved() {
        std::map<int, std::shared_ptr<Entity>> registered;
        auto original = std::make_shared<Entity>();
        registered.emplace(4, original);
        const std::weak_ptr<Entity> expected = original;
        registered.erase(4);
        QVERIFY(!WithRegistered<Entity>(4, expected, [&](int id) {
            const auto it = registered.find(id);
            return it == registered.end() ? std::shared_ptr<Entity>{} : it->second;
        }, [](const std::shared_ptr<Entity> &entity) { ++entity->saves; }));
        QCOMPARE(original->saves, 0);
        QVERIFY(registered.empty());
    }
    void reusedIdDoesNotReceiveOldResult() {
        auto oldEntity = std::make_shared<Entity>();
        auto replacement = std::make_shared<Entity>();
        const std::weak_ptr<Entity> expected = oldEntity;
        QVERIFY(!WithRegistered<Entity>(9, expected,
            [&](int) { return replacement; },
            [](const std::shared_ptr<Entity> &entity) { entity->value = 42; ++entity->saves; }));
        QCOMPARE(oldEntity->saves, 0);
        QCOMPARE(replacement->saves, 0);
        QCOMPARE(replacement->value, 0);
    }
    void registeredObjectReceivesResult() {
        auto entity = std::make_shared<Entity>();
        const std::weak_ptr<Entity> expected = entity;
        QVERIFY(WithRegistered<Entity>(1, expected,
            [&](int id) { return id == 1 ? entity : std::shared_ptr<Entity>{}; },
            [](const std::shared_ptr<Entity> &current) { current->value = 27; ++current->saves; }));
        QCOMPARE(entity->value, 27);
        QCOMPARE(entity->saves, 1);
    }
    void expiredObjectIsIgnored() {
        std::weak_ptr<Entity> expected;
        { auto entity = std::make_shared<Entity>(); expected = entity; }
        bool lookedUp = false, changed = false;
        QVERIFY(!WithRegistered<Entity>(1, expected,
            [&](int) { lookedUp = true; return std::shared_ptr<Entity>{}; },
            [&](const std::shared_ptr<Entity> &) { changed = true; }));
        QVERIFY(!lookedUp);
        QVERIFY(!changed);
    }
    void editedProfileDiscardsSpeedTestResult_data() {
        QTest::addColumn<QString>("field");
        for (const auto &field : {"name", "server", "port", "tls", "type"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void editedProfileDiscardsSpeedTestResult() {
        QFETCH(QString, field);
        auto profile = std::make_shared<ConfigProfile>();
        const std::weak_ptr<ConfigProfile> expected = profile;
        const auto fingerprint = ProfileConfigFingerprint(*profile);
        bool delivered = false;
        bool applied = true;
        std::thread worker([&, expected, fingerprint] {
            PostUi([&, expected, fingerprint] {
                applied = WithRegisteredConfig<ConfigProfile>(1, expected, fingerprint,
                    [&](int) { return profile; },
                    [](const std::shared_ptr<ConfigProfile> &current) {
                        current->latency = 73;
                        current->report = QStringLiteral("old configuration result");
                        ++current->saves;
                    });
                delivered = true;
            });
        });
        worker.join();
        if (field == "type") profile->type = QStringLiteral("vless");
        else if (field == "port") profile->bean->config.insert(field, 8443);
        else if (field == "tls")
            profile->bean->config.insert(field, QJsonObject{{"sni", "new.example"}});
        else profile->bean->config.insert(field, QStringLiteral("changed"));
        QTRY_VERIFY(delivered);
        QVERIFY(!applied);
        QCOMPARE(profile->latency, 0);
        QVERIFY(profile->report.isEmpty());
        QCOMPARE(profile->saves, 0);
    }
    void trafficAndResultsDoNotInvalidateConfigFingerprint() {
        auto profile = std::make_shared<ConfigProfile>();
        const std::weak_ptr<ConfigProfile> expected = profile;
        const auto fingerprint = ProfileConfigFingerprint(*profile);
        profile->traffic.uplink = 100;
        profile->traffic.downlink = 200;
        profile->latency = 42;
        profile->report = QStringLiteral("previous result");
        QVERIFY(WithRegisteredConfig<ConfigProfile>(1, expected, fingerprint,
            [&](int) { return profile; },
            [](const std::shared_ptr<ConfigProfile> &current) {
                current->latency = 73;
                ++current->saves;
            }));
        QCOMPARE(profile->latency, 73);
        QCOMPARE(profile->saves, 1);
        QCOMPARE(profile->traffic.uplink, 100LL);
        QCOMPARE(profile->traffic.downlink, 200LL);
    }
    void unchangedConfigStillRequiresRegisteredIdentity() {
        auto original = std::make_shared<ConfigProfile>();
        const std::weak_ptr<ConfigProfile> expected = original;
        const auto fingerprint = ProfileConfigFingerprint(*original);
        auto replacement = std::make_shared<ConfigProfile>();
        QCOMPARE(ProfileConfigFingerprint(*replacement), fingerprint);
        QVERIFY(!WithRegisteredConfig<ConfigProfile>(1, expected, fingerprint,
            [&](int) { return replacement; },
            [](const std::shared_ptr<ConfigProfile> &current) { ++current->saves; }));
        QCOMPARE(original->saves, 0);
        QCOMPARE(replacement->saves, 0);
        QVERIFY(!WithRegisteredConfig<ConfigProfile>(1, expected, fingerprint,
            [](int) { return std::shared_ptr<ConfigProfile>{}; },
            [](const std::shared_ptr<ConfigProfile> &current) { ++current->saves; }));
        QCOMPARE(original->saves, 0);
    }
    void duplicateTagsAreQueriedOnce() {
        std::map<std::string, int> calls;
        const auto deltas = QueryUniqueTags({"proxy", "proxy", "chain", "bypass", "chain"},
            [&](const std::string &tag, const char *direction) {
                ++calls[tag + "/" + direction];
                return std::string(direction) == "uplink" ? 11LL : 23LL;
            });
        QCOMPARE(static_cast<int>(deltas.size()), 3);
        QCOMPARE(static_cast<int>(calls.size()), 6);
        for (const auto &entry : calls) QCOMPARE(entry.second, 1);
        QCOMPARE(deltas.at("proxy").uplink, 11LL);
        QCOMPARE(deltas.at("proxy").downlink, 23LL);
    }
    void trafficApplicationAddsToCurrentTotals() {
        Counters counters;
        counters.uplink = 100; counters.downlink = 200;
        ApplyTrafficDelta(counters, TrafficDelta{10, 30}, 500);
        QCOMPARE(counters.uplink, 110LL);
        QCOMPARE(counters.downlink, 230LL);
        QCOMPARE(counters.uplink_rate, 20LL);
        QCOMPARE(counters.downlink_rate, 60LL);
    }
    void twoTargetsSharingTagReceiveSameDelta() {
        int queries = 0;
        const auto deltas = QueryUniqueTags({"shared", "shared"},
            [&](const std::string &, const char *) { ++queries; return 7LL; });
        Counters first, second;
        ApplyTrafficDelta(first, deltas.at("shared"), 1000);
        ApplyTrafficDelta(second, deltas.at("shared"), 1000);
        QCOMPARE(queries, 2);
        QCOMPARE(first.uplink, 7LL); QCOMPARE(second.uplink, 7LL);
        QCOMPARE(first.downlink, 7LL); QCOMPARE(second.downlink, 7LL);
    }
    void zeroIntervalDoesNotDivideByZero() {
        Counters counters;
        ApplyTrafficDelta(counters, TrafficDelta{2, 3}, 0);
        QCOMPARE(counters.uplink_rate, 2000LL);
        QCOMPARE(counters.downlink_rate, 3000LL);
    }
    void workerPacketIsAppliedAfterWorkerFinishes() {
        Counters counters;
        std::atomic<bool> workerFinished{false};
        bool applied = false;
        std::thread worker([&] {
            const auto deltas = QueryUniqueTags({"proxy"},
                [](const std::string &, const char *) { return 13LL; });
            PostUi([deltas, &counters, &applied] {
                RequireUiThread();
                ApplyTrafficDelta(counters, deltas.at("proxy"), 1000);
                applied = true;
            });
            workerFinished.store(true);
        });
        worker.join();
        QVERIFY(workerFinished.load()); QVERIFY(!applied);
        QTRY_VERIFY(applied);
        QCOMPARE(counters.uplink, 13LL); QCOMPARE(counters.downlink, 13LL);
    }
    void resetDiscardsInFlightSampleIncludingZeroCounters() {
        TrafficGeneration generation;
        TrafficGeneration other;
        Counters counters;
        const auto before = generation.Snapshot();
        const auto unaffected = other.Snapshot();
        std::thread worker([&] {
            const auto deltas = QueryUniqueTags({"proxy"},
                [](const std::string &, const char *) { return 13LL; });
            PostUi([&, before, deltas] {
                if (generation.Accepts(before))
                    ApplyTrafficDelta(counters, deltas.at("proxy"), 1000);
            });
        });
        worker.join();
        generation.Reset();
        QCoreApplication::processEvents();
        QCOMPARE(counters.uplink, 0LL); QCOMPARE(counters.downlink, 0LL);
        QVERIFY(!generation.Accepts(before));
        QVERIFY(other.Accepts(unaffected));
        const auto after = generation.Snapshot();
        QVERIFY(generation.Accepts(after));
        ApplyTrafficDelta(counters, TrafficDelta{3, 5}, 1000);
        QCOMPARE(counters.uplink, 3LL); QCOMPARE(counters.downlink, 5LL);
    }
    void snapshotUsesMergedTransactionInterface() {
        QObject context;
        initializeUiThreadDispatcher(&context);
        std::optional<int> received;
        std::atomic<bool> finished{false};
        std::thread worker([&] {
            received = ReadUi<int>([] { RequireUiThread(); return 73; });
            finished.store(true);
        });
        // Join before asserting so a failed check cannot destroy a live thread.
        while (!finished.load()) { QCoreApplication::processEvents(); QThread::msleep(1); }
        worker.join();
        QVERIFY(received.has_value()); QCOMPARE(*received, 73);
        shutdownUiThreadDispatcher();
    }
    void wrongThreadFailsInReleaseToo() {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("NKR_W03B_THREAD_PROBE", "1");
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), QStringList{});
        QVERIFY(child.waitForStarted(5000)); QVERIFY(child.waitForFinished(10000));
        QVERIFY(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0);
        QVERIFY(child.readAllStandardError().contains("W03: model access outside the application thread"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("NKR_W03B_THREAD_PROBE") == "1") {
        std::thread worker([] { RequireUiThread(); });
        worker.join(); return 0;
    }
    W03bTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_w03b.moc"
