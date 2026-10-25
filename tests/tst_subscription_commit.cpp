#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QSemaphore>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "db/Database.hpp"
#include "db/ProfileFilter.hpp"
#include "main/NekoGui_UiThread_p.hpp"
#include "sub/SubscriptionCommit.hpp"

using namespace NekoGui;
namespace S = NekoGui_sub::Subscription;

namespace {
void Require(bool condition) {
    if (!condition) qFatal("PR-4 fixture or synchronization failure");
}
void Acquire(QSemaphore &semaphore) {
    Require(semaphore.tryAcquire(1, 10000));
}
QByteArray Read(const QString &path) {
    QFile file(path);
    Require(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void Write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    Require(file.write(bytes) == bytes.size());
    Require(file.flush());
}
S::Entities Nodes(const QStringList &names) {
    S::Entities result;
    for (const auto &name : names) {
        auto ent = ProfileManager::NewProxyEntity("socks");
        ent->bean->name = name;
        ent->bean->serverAddress = name + ".test";
        ent->bean->serverPort = 1080;
        result.append(ent);
    }
    return result;
}
QStringList Names(const S::Entities &entities) {
    QStringList result;
    for (const auto &ent : entities) result.append(ent->bean->name);
    return result;
}
QList<int> Ids(const S::Entities &entities) {
    QList<int> result;
    for (const auto &ent : entities) result.append(ent->id);
    return result;
}
bool Unique(const QList<int> &ids) {
    QSet<int> seen;
    for (const int id : ids) {
        if (id < 0 || seen.contains(id)) return false;
        seen.insert(id);
    }
    return true;
}
struct ChildProcess {
    QProcess process;
    ~ChildProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
};
int CrashChild(const QString &directory, const QString &cut) {
    Require(QDir::setCurrent(directory));
    dataStore->flag_reorder = false;
    dataStore->started_id = -1;
    Require(profileManager->LoadManager());
    auto group = profileManager->GetGroup(0);
    Require(bool(group));
    S::Io io;
    io.checkpoint = [cut](const QString &name) {
        if (name != cut) return;
        std::fputs("PR4-CUT\n", stdout);
        std::fflush(stdout);
        QSemaphore parked;
        parked.acquire();
    };
    S::Commit(group, Nodes({"c", "d"}), true, false, "new", io);
    return 9;
}
} // namespace

class SubscriptionCommitTest : public QObject {
    Q_OBJECT
    QString previousDirectory;
    ProfileManager *previousManager = nullptr;
    DataStore *previousStore = nullptr;
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<ProfileManager> manager;
    std::unique_ptr<DataStore> store;
    S::GroupPtr group;

    void seed(int layout, const QStringList &names = {"a", "b"}) {
        group = ProfileManager::NewGroup();
        group->name = "fixture";
        group->url = "https://example.invalid/sub";
        group->info.clear();
        group->sub_last_update = 101;
        group->skip_auto_update = true;
        Require(manager->AddGroup(group));
        Require(group->id == 0);
        const auto entries = Nodes(names);
        int index = 0;
        for (const auto &ent : entries) {
            ent->traffic_data->uplink = 100 + index;
            ent->traffic_data->downlink = 200 + index;
            Require(manager->AddProfile(ent, group->id));
            ++index;
        }
        group->order_explicit = layout == 1 || layout == 3;
        group->order = group->order_explicit ? Ids(entries) : QList<int>{};
        Require(group->SaveChecked().ok);
        manager->SaveManager();
        Require(QFile::exists("groups/pm.json"));
    }
    void reload() {
        group.reset();
        manager.reset(new ProfileManager);
        profileManager = manager.get();
        dataStore->flag_reorder = false;
        Require(manager->LoadManager());
        group = manager->GetGroup(0);
        Require(bool(group));
    }
private slots:
    void init() {
        previousDirectory = QDir::currentPath();
        temporary.reset(new QTemporaryDir);
        QVERIFY(temporary->isValid());
        QVERIFY(QDir::setCurrent(temporary->path()));
        QVERIFY(QDir().mkdir("profiles"));
        QVERIFY(QDir().mkdir("groups"));
        previousManager = profileManager;
        previousStore = dataStore;
        manager.reset(new ProfileManager);
        store.reset(new DataStore);
        profileManager = manager.get();
        dataStore = store.get();
        dataStore->current_group = 0;
        dataStore->started_id = -1;
        dataStore->flag_reorder = false;
    }
    void cleanup() {
        group.reset();
        profileManager = previousManager;
        dataStore = previousStore;
        manager.reset();
        store.reset();
        QVERIFY(QDir::setCurrent(previousDirectory));
        temporary.reset();
    }
    void schemaAndLegacyLoading() {
        seed(0);
        const auto allIds = Ids(group->Profiles());
        auto legacy = group->ToJson();
        legacy.remove("order_explicit");
        legacy.remove("recovery_required");
        Write(group->fn, QJsonDocument(legacy).toJson());
        reload();
        QVERIFY(!group->order_explicit);
        QVERIFY(!group->recoveryRequired);
        QCOMPARE(Ids(group->ProfilesWithOrder()), allIds);
        group->order = {allIds.at(1)};
        QVERIFY(group->SaveChecked().ok);
        reload();
        QVERIFY(!group->order_explicit);
        QCOMPARE(Ids(group->ProfilesWithOrder()), QList<int>{allIds.at(1)});
        group->order.clear();
        group->order_explicit = true;
        group->recoveryRequired = true;
        QVERIFY(group->SaveChecked().ok);
        reload();
        QVERIFY(group->ProfilesWithOrder().isEmpty());
        QCOMPARE(int(group->Profiles().size()), 2);
        QVERIFY(group->recoveryRequired);
        const auto snapshot = group->ToJsonBytes();
        auto invalid = group->ToJson();
        invalid.insert("order_explicit", 1);
        QVERIFY(!group->FromJsonBytes(QJsonDocument(invalid).toJson()));
        QCOMPARE(group->ToJsonBytes(), snapshot);
        invalid = group->ToJson();
        invalid.insert("recovery_required", "true");
        QVERIFY(!group->FromJsonBytes(QJsonDocument(invalid).toJson()));
        QCOMPARE(group->ToJsonBytes(), snapshot);
        auto other = ProfileManager::NewGroup();
        other->name = "other";
        QVERIFY(manager->AddGroup(other));
        auto foreign = Nodes({"foreign"}).first();
        QVERIFY(manager->AddProfile(foreign, other->id));
        group->order = {allIds.at(0), allIds.at(0), foreign->id, 99999};
        QCOMPARE(Ids(group->ProfilesWithOrder()), QList<int>{allIds.at(0)});
    }
    void parserRejectsWithoutMutation_data() {
        QTest::addColumn<QString>("content");
        QTest::newRow("empty") << QString();
        QTest::newRow("unsupported") << QString("not a subscription");
#ifndef NKR_NO_YAML
        QTest::newRow("invalid-yaml") << QString("proxies: [");
        QTest::newRow("exception-after-valid-entry") << QString(
            "proxies:\n"
            "  - {type: socks5, name: a, server: a.test, port: 1080}\n"
            "  - 7\n");
#endif
    }
    void parserRejectsWithoutMutation() {
        QFETCH(QString, content);
        seed(0);
        const auto memory = group->ToJsonBytes();
        const auto disk = Read(group->fn);
        const auto profiles = Ids(group->Profiles());
        const auto reply = S::Apply(group, content, true, true, "new");
        QCOMPARE(int(reply.status), int(S::Status::Rejected));
        QCOMPARE(group->ToJsonBytes(), memory);
        QCOMPARE(Read(group->fn), disk);
        QCOMPARE(Ids(group->Profiles()), profiles);
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "b"}));
    }
    void parserStagesAndSkipsUnsupportedLines() {
        seed(0);
        const auto disk = Read(group->fn);
        S::Entities staged;
        QVERIFY(S::Parse("socks5://a.test:1080#a\nunsupported-line\nsocks5://b.test:1080#b", staged));
        QCOMPARE(int(staged.size()), 2);
        for (const auto &ent : staged) {
            QCOMPARE(ent->id, -1);
            QVERIFY(ent->fn.isEmpty());
        }
        QCOMPARE(int(manager->profiles.size()), 2);
        QCOMPARE(Read(group->fn), disk);
        QVERIFY(S::Succeeded(S::Commit(group, staged, true, false, "new")));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "b"}));
    }
    void unchangedRetainsEveryIdAndTraffic_data() {
        QTest::addColumn<bool>("running");
        QTest::newRow("stopped") << false;
        QTest::newRow("first-running") << true;
    }
    void unchangedRetainsEveryIdAndTraffic() {
        QFETCH(bool, running);
        seed(0);
        const auto original = group->ProfilesWithOrder();
        const auto originalIds = Ids(original);
        if (running) dataStore->started_id = originalIds.first();
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto reply = S::Commit(group, Nodes({"a", "b"}), true, false, "new");
            QCOMPARE(int(reply.status), int(S::Status::Committed));
            QCOMPARE(Ids(group->ProfilesWithOrder()), originalIds);
            QVERIFY(Unique(group->order));
            QCOMPARE(int(manager->profiles.size()), 2);
            const auto current = group->ProfilesWithOrder();
            for (int i = 0; i < current.size(); ++i) {
                QCOMPARE(current.at(i), original.at(i));
                QCOMPARE(current.at(i)->traffic_data->uplink, static_cast<long long>(100 + i));
                QCOMPARE(current.at(i)->traffic_data->downlink, static_cast<long long>(200 + i));
            }
        }
        reload();
        QCOMPARE(Ids(group->ProfilesWithOrder()), originalIds);
        QCOMPARE(group->ProfilesWithOrder().first()->traffic_data->uplink, 100LL);
        QVERIFY(!group->recoveryRequired);
    }
    void commonConsumesEachSourceOnce() {
        const auto source = Nodes({"a", "a"});
        const auto destination = Nodes({"a", "a", "a"});
        S::Entities oldMatches, newMatches;
        ProfileFilter::Common(
            {source.at(0), source.at(0), source.at(1)},
            {destination.at(0), destination.at(0), destination.at(1), destination.at(2)},
            oldMatches, newMatches, false);
        QCOMPARE(int(oldMatches.size()), 2);
        QCOMPARE(oldMatches.at(0), source.at(0));
        QCOMPARE(oldMatches.at(1), source.at(1));
        QCOMPARE(newMatches.at(0), destination.at(0));
        QCOMPARE(newMatches.at(1), destination.at(1));
    }
    void duplicateClearAndRunningPolicies_data() {
        QTest::addColumn<bool>("clear");
        QTest::addColumn<int>("runningIndex");
        for (const bool clear : {false, true}) {
            for (const int index : {-1, 1, 2}) {
                const auto name = QString("%1-%2").arg(clear).arg(index).toUtf8();
                QTest::newRow(name.constData()) << clear << index;
            }
        }
    }
    void duplicateClearAndRunningPolicies() {
        QFETCH(bool, clear);
        QFETCH(int, runningIndex);
        seed(1, {"a", "a", "b"});
        const auto old = group->ProfilesWithOrder();
        const auto oldIds = Ids(old);
        if (runningIndex >= 0) dataStore->started_id = old.at(runningIndex)->id;
        const auto reply = S::Commit(group, Nodes({"a", "a", "c"}), true, clear, "new");
        QCOMPARE(int(reply.status), int(S::Status::Committed));
        const auto visible = group->ProfilesWithOrder();
        const QStringList expected = runningIndex == 2 ? QStringList{"a", "c", "b"} : QStringList{"a", "c"};
        QCOMPARE(Names(visible), expected);
        QVERIFY(Unique(group->order));
        QCOMPARE(int(manager->profiles.size()), int(expected.size()));
        if (runningIndex == 1) {
            QCOMPARE(visible.first(), old.at(1));
            QCOMPARE(visible.first()->traffic_data->uplink, 101LL);
        } else if (!clear) {
            QCOMPARE(visible.first(), old.at(0));
        } else {
            QVERIFY(!oldIds.contains(visible.first()->id));
        }
        if (runningIndex == 2) {
            QCOMPARE(visible.last(), old.at(2));
            QCOMPARE(visible.last()->traffic_data->uplink, 102LL);
        }
        if (!clear) {
            const auto firstIds = Ids(visible);
            QVERIFY(S::Succeeded(S::Commit(group, Nodes({"a", "a", "c"}), true, false, "new")));
            QCOMPARE(Ids(group->ProfilesWithOrder()), firstIds);
        }
    }
    void ordinaryImportsAndMovesRespectExplicitEmpty() {
        seed(3, {});
        const auto oldTime = group->sub_last_update;
        const auto oldInfo = group->info;
        QVERIFY(S::Succeeded(S::Commit(group, Nodes({"a", "a"}), false, false, "ignored")));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "a"}));
        QCOMPARE(group->sub_last_update, oldTime);
        QCOMPARE(group->info, oldInfo);
        QVERIFY(S::Succeeded(S::Commit(group, Nodes({"b"}), false, false, "ignored")));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "a", "b"}));
        auto destination = ProfileManager::NewGroup();
        destination->name = "destination";
        destination->order_explicit = true;
        QVERIFY(manager->AddGroup(destination));
        const auto moved = group->ProfilesWithOrder().last();
        manager->MoveProfile(moved, destination->id);
        QCOMPARE(Names(destination->ProfilesWithOrder()), QStringList{"b"});
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "a"}));
        manager->MoveProfile(moved, group->id);
        QVERIFY(destination->ProfilesWithOrder().isEmpty());
        QVERIFY(destination->order_explicit);
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "a", "b"}));
    }
    void failures_data() {
        QTest::addColumn<int>("layout");
        QTest::addColumn<QString>("fault");
        const QStringList faults{"pin", "add-1", "add-2", "publish", "rollback-remove", "rollback-save",
                                 "cleanup-remove", "cleanup-marker", "exception"};
        for (int layout = 0; layout < 4; ++layout) {
            for (const auto &fault : faults) {
                if (layout >= 2 && fault == "cleanup-remove") continue;
                const auto name = QString("%1-%2").arg(layout).arg(fault).toUtf8();
                QTest::newRow(name.constData()) << layout << fault;
            }
        }
    }
    void failures() {
        QFETCH(int, layout);
        QFETCH(QString, fault);
        const QStringList oldNames = layout >= 2 ? QStringList{} : QStringList{"a", "b"};
        seed(layout, oldNames);
        const auto snapshot = group->ToJsonBytes();
        const auto disk = Read(group->fn);
        S::Io io;
        const auto realAdd = io.add;
        const auto realSave = io.save;
        const auto realRemove = io.remove;
        int adds = 0, saves = 0, removes = 0;
        io.add = [&](const S::Entity &ent, int gid) {
            ++adds;
            const bool fail = (fault == "add-1" && adds == 1) || (fault == "add-2" && adds == 2);
            const bool previous = ent->save_control_no_save;
            ent->save_control_no_save = fail;
            const bool result = realAdd(ent, gid);
            ent->save_control_no_save = previous;
            return result;
        };
        io.save = [&](const S::GroupPtr &target) {
            ++saves;
            const bool fail = (fault == "pin" && saves == 1) ||
                ((fault == "publish" || fault == "rollback-remove" || fault == "rollback-save") && saves == 2) ||
                (fault == "rollback-save" && saves == 3) || (fault == "cleanup-marker" && saves == 3);
            const bool previous = target->save_control_no_save;
            target->save_control_no_save = fail;
            const bool result = realSave(target);
            target->save_control_no_save = previous;
            return result;
        };
        io.remove = [&](int id) {
            ++removes;
            if ((fault == "rollback-remove" || fault == "cleanup-remove") && removes == 1) return false;
            return realRemove(id);
        };
        io.checkpoint = [&](const QString &name) {
            if (fault == "exception" && name == "added-1") throw std::runtime_error("injected checkpoint failure");
        };
        const auto reply = S::Commit(group, Nodes({"c", "d"}), true, false, "new", io);
        const bool published = fault == "cleanup-remove" || fault == "cleanup-marker";
        const bool recoveryFailed = fault == "rollback-remove" || fault == "rollback-save";
        const auto expectedStatus = published ? S::Status::CommittedCleanupPending :
            recoveryFailed ? S::Status::RecoveryFailed : S::Status::Failed;
        QCOMPARE(int(reply.status), int(expectedStatus));
        const QStringList expected = published ? QStringList{"c", "d"} : oldNames;
        QCOMPARE(Names(group->ProfilesWithOrder()), expected);
        if (!published) {
            QCOMPARE(group->info, QString());
            QCOMPARE(group->sub_last_update, qint64(101));
            QVERIFY(group->skip_auto_update);
        }
        if (recoveryFailed || published) {
            QVERIFY(group->order_explicit);
            QVERIFY(group->recoveryRequired);
        } else {
            QCOMPARE(group->ToJsonBytes(), snapshot);
            QCOMPARE(Read(group->fn), disk);
        }
        if (fault == "cleanup-remove") QCOMPARE(int(manager->profiles.size()), 3);
        reload();
        QCOMPARE(Names(group->ProfilesWithOrder()), expected);
        QCOMPARE(group->recoveryRequired, recoveryFailed || published);
        QCOMPARE(int(S::Commit(group, Nodes({"c", "d"}), true, false, "new").status), int(S::Status::Committed));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"c", "d"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        QVERIFY(!group->recoveryRequired);
        reload();
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"c", "d"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        QVERIFY(!group->recoveryRequired);
    }
    void wrongThreadAndUnavailableTargetDoNotMutate() {
        seed(0);
        const auto snapshot = group->ToJsonBytes();
        const auto disk = Read(group->fn);
        S::Reply wrongThread;
        std::thread worker([&] { wrongThread = S::Commit(group, Nodes({"c"}), true, false, "new"); });
        worker.join();
        QCOMPARE(int(wrongThread.status), int(S::Status::Failed));
        QCOMPARE(group->ToJsonBytes(), snapshot);
        QCOMPARE(Read(group->fn), disk);
        group->archive = true;
        QCOMPARE(int(S::Commit(group, Nodes({"c"}), true, false, "new").status), int(S::Status::Rejected));
        group->archive = false;
        auto unregistered = ProfileManager::NewGroup();
        unregistered->id = group->id;
        QCOMPARE(int(S::Commit(unregistered, Nodes({"c"}), true, false, "new").status), int(S::Status::Rejected));
        QCOMPARE(group->ToJsonBytes(), snapshot);
    }
    void cancelledDispatchCannotCommitLater() {
        seed(0);
        const auto snapshot = Read(group->fn);
        std::atomic<int> calls{0};
        S::Reply reply;
        std::thread worker([&] {
            reply = S::Dispatch([&, ownedGroup = group, staged = Nodes({"c", "d"})] {
                ++calls;
                return S::Commit(ownedGroup, staged, true, false, "new");
            }, 0, UiCommitRunningBudgetMs);
        });
        worker.join(); // Deliberately do not service the UI queue before cancellation.
        QCOMPARE(int(reply.status), int(S::Status::Cancelled));
        QCoreApplication::processEvents();
        QCOMPARE(calls.load(), 0);
        QCOMPARE(Read(group->fn), snapshot);
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "b"}));
    }
    void runningCommitMustFinishAfterTimeoutDecision() {
        seed(0);
        NekoGui_detail::UiTask queued;
        QSemaphore posted, entered, cancellationLost;
        std::atomic<bool> returned{false};
        bool prematureReturn = false;
        NekoGui_detail::UiThreadDispatcher dispatcher(QThread::currentThread(), [&](NekoGui_detail::UiTask task) {
            queued = std::move(task);
            posted.release();
            return true;
        });
        NekoGui_detail::UiWaitHooks hooks;
        hooks.firstWait = [&](QSemaphore &, int) { Acquire(entered); return false; };
        hooks.cancelLost = [&] { cancellationLost.release(); };
        S::Io io;
        io.checkpoint = [&](const QString &name) {
            if (name != "pinned") return;
            entered.release();
            Acquire(cancellationLost);
            prematureReturn = returned.load();
        };
        UiThreadResult result = UiThreadResult::Failed;
        std::thread worker([&] {
            result = dispatcher.call([&] {
                return S::UiResult(S::Commit(group, Nodes({"c", "d"}), true, false, "new", io));
            }, 0, UiCommitRunningBudgetMs, hooks);
            returned.store(true);
        });
        Acquire(posted);
        queued();
        worker.join();
        QVERIFY(!prematureReturn);
        QVERIFY(returned.load());
        QCOMPARE(int(result), int(UiThreadResult::Committed));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"c", "d"}));
        QVERIFY(!group->recoveryRequired);
    }
    void crashes_data() {
        QTest::addColumn<int>("layout");
        QTest::addColumn<QString>("cut");
        for (int layout = 0; layout < 4; ++layout) {
            QStringList cuts{"before-pin", "pinned", "added-1", "added-2", "published", "cleaned"};
            if (layout < 2) cuts << "deleted-1" << "deleted-2";
            for (const auto &cut : cuts) {
                const auto name = QString("%1-%2").arg(layout).arg(cut).toUtf8();
                QTest::newRow(name.constData()) << layout << cut;
            }
        }
    }
    void crashes() {
        QFETCH(int, layout);
        QFETCH(QString, cut);
        const QStringList oldNames = layout >= 2 ? QStringList{} : QStringList{"a", "b"};
        seed(layout, oldNames);
        ChildProcess child;
        child.process.setProgram(QCoreApplication::applicationFilePath());
        child.process.setArguments({"--pr4-crash", temporary->path(), cut});
        child.process.start();
        QVERIFY(child.process.waitForStarted(10000));
        QByteArray output;
        QElapsedTimer timer;
        timer.start();
        while (!output.contains("PR4-CUT\n") && child.process.state() != QProcess::NotRunning && timer.elapsed() < 10000) {
            child.process.waitForReadyRead(100);
            output += child.process.readAllStandardOutput();
        }
        const bool reached = output.contains("PR4-CUT\n");
        child.process.kill();
        QVERIFY(child.process.waitForFinished(5000));
        QVERIFY2(reached, child.process.readAllStandardError().constData());
        QCOMPARE(child.process.exitStatus(), QProcess::CrashExit);
        reload();
        const bool published = cut == "published" || cut == "cleaned" || cut.startsWith("deleted-");
        const QStringList expected = published ? QStringList{"c", "d"} : oldNames;
        QCOMPARE(Names(group->ProfilesWithOrder()), expected);
        QCOMPARE(group->recoveryRequired, cut != "before-pin" && cut != "cleaned");
        if (!published) {
            QCOMPARE(group->info, QString());
            QCOMPARE(group->sub_last_update, qint64(101));
        } else {
            QCOMPARE(group->info, QString("new"));
        }
        QCOMPARE(int(S::Commit(group, Nodes({"c", "d"}), true, false, "new").status), int(S::Status::Committed));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"c", "d"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        reload();
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"c", "d"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        QVERIFY(!group->recoveryRequired);
    }
    void legacyReorderPreservesHiddenRegisteredProfiles() {
        seed(1, {"visible", "hidden"});
        group->order = {group->Profiles().first()->id};
        group->recoveryRequired = true;
        QVERIFY(group->SaveChecked().ok);
        manager->SaveManager();
        manager.reset(new ProfileManager);
        profileManager = manager.get();
        dataStore->flag_reorder = true;
        // Reorder reports completion with a modal dialog. Dismiss that real UI
        // notification from its nested event loop so this regression is unattended.
        bool dismissed = false;
        QTimer::singleShot(0, [&dismissed] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog) return;
            dismissed = true;
            dialog->accept();
        });
        QVERIFY(manager->LoadManager());
        QVERIFY(dismissed);
        group = manager->GetGroup(0);
        QVERIFY(bool(group));
        QCOMPARE(Names(group->Profiles()), QStringList({"visible", "hidden"}));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"visible", "hidden"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        QVERIFY(!group->order_explicit);
        QVERIFY(!group->recoveryRequired);
        for (const auto &ent : group->Profiles()) QVERIFY(QFile::exists(ent->fn));
        reload();
        QCOMPARE(Names(group->Profiles()), QStringList({"visible", "hidden"}));
    }
};

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    initializeUiThreadDispatcher(&application);
    MW_show_log = [](QString) {};
    MW_dialog_message = [](QString, QString) {};
    const auto arguments = application.arguments();
    if (arguments.size() == 4 && arguments.at(1) == "--pr4-crash") return CrashChild(arguments.at(2), arguments.at(3));
    SubscriptionCommitTest test;
    const int result = QTest::qExec(&test, argc, argv);
    shutdownUiThreadDispatcher();
    return result;
}

#include "tst_subscription_commit.moc"
