#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <memory>
#include "db/Database.hpp"
#include "db/Renumber.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "main/NekoGui_UiThread_p.hpp"

namespace {
using Renumber::Files;
struct CurrentDirectory {
    QString previous = QDir::currentPath();
    explicit CurrentDirectory(const QString &path) {
        Renumber::Require(QDir::setCurrent(path), "Cannot enter test directory");
    }
    ~CurrentDirectory() { QDir::setCurrent(previous); }
};
QByteArray Json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject Object(const QByteArray &bytes) { return QJsonDocument::fromJson(bytes).object(); }
void Put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    Renumber::Require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Test write open failed");
    Renumber::Require(file.write(bytes) == bytes.size() && file.flush(), "Test write failed");
}
void Seed(const QString &root) {
    CurrentDirectory cwd(root);
    Renumber::Require(QDir().mkdir("profiles") && QDir().mkdir("groups") && QDir().mkdir("groups/corrupt"), "Cannot seed directories");
    for (int gid : {30, 8, 50, 70}) {
        QJsonArray order;
        if (gid == 30) order.append(41);
        if (gid == 8) order.append(7);
        Put(QString("groups/%1.json").arg(gid), Json(QJsonObject{
            {"id", gid}, {"name", QString("group-%1").arg(gid)}, {"order", order},
            {"order_explicit", gid != 70}, {"recovery_required", gid == 8}, {"front_proxy_id", gid == 30 ? 90 : -1}}));
    }
    const QList<QPair<int, int>> members{{41, 30}, {7, 8}, {90, 8}, {99, 50}, {120, 70}};
    for (const auto &member : members) {
        QJsonObject bean{{"_v", 0}};
        if (member.first == 41) bean["list"] = QJsonArray{7, 90};
        Put(QString("profiles/%1.json").arg(member.first), Json(QJsonObject{
            {"id", member.first}, {"gid", member.second}, {"type", member.first == 41 ? "chain" : "socks"}, {"bean", bean},
            {"identity", QString("profile-%1").arg(member.first)},
            {"traffic", QJsonObject{{"dl", 1000 + member.first}, {"ul", 2000 + member.first}}},
            {"yc", member.first}, {"report", QString("report-%1").arg(member.first)}}));
    }
    Put("groups/pm.json", Json(QJsonObject{{"groups", QJsonArray{30, 8, 50, 70}}, {"unknown-manager-field", "preserve"}}));
    Put("groups/nekobox.json", Json(QJsonObject{{"current_group", 8}, {"remember_id", 90}, {"remember_enable", true},
                                              {"unknown-settings-field", "preserve"}}));
    Put("groups/corrupt/evidence.corrupt", "{broken");
    Put("profiles/notes.txt", "preserve auxiliary file");
}
// Copy only stopped test fixtures, including the immutable journal and partial
// io.tmp. No application writer or lease remains alive during these copies.
void CopyCheckpoint(const QString &from, const QString &to) {
    const auto entries = QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &entry : entries) {
        Renumber::Require(!entry.isSymLink(), "Unexpected checkpoint symlink");
        const auto destination = to + "/" + entry.fileName();
        if (entry.isDir()) {
            Renumber::Require(QDir().mkdir(destination), "Cannot copy checkpoint directory");
            CopyCheckpoint(entry.filePath(), destination);
        } else {
            Renumber::Require(entry.isFile() && QFile::copy(entry.filePath(), destination), "Cannot copy checkpoint file");
            Renumber::Require(Renumber::Read(entry.filePath()) == Renumber::Read(destination), "Checkpoint copy differs");
        }
    }
}
Files Tree(const QString &root) { CurrentDirectory cwd(root); return Renumber::Scan(); }
Files Expected(const Files &oldFiles) { return NekoGui::BuildRenumbered(oldFiles); }
struct ChildResult { int code = -1; QByteArray output; QStringList points; };
ChildResult Run(const QString &root, const QString &mode, int cut = -1, const QString &kind = "crash") {
    QProcess child;
    child.setWorkingDirectory(root);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {"--worker", mode, QString::number(cut), kind});
    Renumber::Require(child.waitForStarted(10000), "Worker did not start");
    Renumber::Require(child.waitForFinished(60000), "Worker timed out");
    ChildResult result;
    result.code = child.exitCode();
    result.output = child.readAll();
    for (const auto &line : result.output.split('\n'))
        if (line.startsWith("POINT\t")) result.points.append(QString::fromUtf8(line.mid(6)));
    return result;
}
bool Committed(const QString &root) {
    return QFileInfo::exists(root + "/.renumber/COMMIT") || !QDir(root).entryList(
        QStringList{".renumber-done-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
}
int IndexOf(const QStringList &points, const QString &point) {
    const int index = points.indexOf(point);
    Renumber::Require(index >= 0, "Missing injection point: " + point);
    return index;
}
void CorruptBundle() {
    auto bytes = Renumber::Read(".renumber/bundle.json");
    Renumber::Require(!bytes.isEmpty(), "Missing corruption target");
    bytes[bytes.size() / 2] = char(bytes.at(bytes.size() / 2) ^ 1);
    Put(".renumber/bundle.json", bytes);
}
int Worker(const QStringList &arguments) {
    const auto mode = arguments.at(2);
    const int cut = arguments.at(3).toInt();
    const auto kind = arguments.at(4);
    int index = -1;
    bool failNext = false;
    Renumber::Hooks hooks;
    hooks.point = [&](const QString &point) {
        ++index;
        std::cout << "POINT\t" << point.toStdString() << std::endl;
        if ((kind == "corrupt-backup" && point == "rename:publish:after") ||
            (kind == "corrupt-stage" && point == "state:BACKUP_DONE")) CorruptBundle();
        if (index == cut) {
            if (kind == "fail") failNext = true;
            else std::_Exit(86); // Abrupt process exit without unwinding or orderly shutdown.
        }
    };
    hooks.fail = [&](const QString &) { const bool fail = failNext; failNext = false; return fail; };
    try {
        Renumber::Lease lease;
        if (mode == "hold-exclusive") lease.Begin(true);
        else NekoGui::PrepareConfiguration(mode == "start", lease, hooks);
        if (mode.startsWith("hold-")) { std::cout << "READY" << std::endl; std::cin.get(); }
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << std::endl; return 2; }
}
struct Globals {
    NekoGui::ProfileManager *previousManager = NekoGui::profileManager;
    NekoGui::DataStore *previousStore = NekoGui::dataStore;
    NekoGui::ProfileManager manager;
    NekoGui::DataStore store;
    Globals() {
        NekoGui::profileManager = &manager;
        NekoGui::dataStore = &store;
        store.fn = "groups/nekobox.json";
    }
    ~Globals() { NekoGui::profileManager = previousManager; NekoGui::dataStore = previousStore; }
};
bool WaitReady(QProcess &process) {
    QByteArray output;
    for (int i = 0; i < 100; ++i) {
        process.waitForReadyRead(100);
        output += process.readAll();
        if (output.contains("READY")) return true;
        if (process.state() == QProcess::NotRunning) return false;
    }
    return false;
}
} // namespace

class RenumberTest : public QObject {
    Q_OBJECT
private slots:
    void nativeSyncStaticCheck() {
        const auto source = Renumber::Read(QStringLiteral(NKR_RENUMBER_SOURCE_DIR) + "/db/Renumber.cpp");
        const auto header = Renumber::Read(QStringLiteral(NKR_RENUMBER_SOURCE_DIR) + "/db/Renumber.hpp");
        const int start = source.indexOf("bool NativeSync(int fd) {");
        const int end = source.indexOf("\n}", start);
        QVERIFY(start >= 0 && end > start);
        const auto helper = source.mid(start, end + 2 - start);
        auto outside = source;
        outside.remove(start, end + 2 - start);
        const QRegularExpression bare("::\\s*fsync\\s*\\(");
        QVERIFY(!bare.match(QString::fromUtf8(outside)).hasMatch());
        QVERIFY(!bare.match(QString::fromUtf8(header)).hasMatch());
        QVERIFY(helper.contains("#elif defined(__APPLE__)"));
        QVERIFY(helper.contains("::fcntl(fd, F_FULLFSYNC) == 0"));
        QCOMPARE(helper.count("::fsync("), 1);
        QVERIFY(source.contains("const bool ok = NativeSync(fd);"));
        QVERIFY(source.contains("return NativeSync(file.handle());"));
        qInfo("nativeSyncStaticCheck: PASS; bare ::fsync calls outside NativeSync = 0; Apple F_FULLFSYNC required");
    }
    void nativeSyncFailuresAreFatal() {
        QTemporaryDir baseline;
        Seed(baseline.path());
        const auto trace = Run(baseline.path(), "start");
        QVERIFY2(trace.code == 0, trace.output.constData());
        int fileFailures = 0, directoryFailures = 0;
        for (int cut = 0; cut < trace.points.size(); ++cut) {
            const auto point = trace.points.at(cut);
            if (!point.endsWith(":before") || (!point.startsWith("filesync:") && !point.startsWith("dirsync:"))) continue;
            QTemporaryDir root;
            Seed(root.path());
            const auto oldFiles = Tree(root.path());
            const auto newFiles = Expected(oldFiles);
            const auto result = Run(root.path(), "start", cut, "fail");
            QVERIFY2(result.code == 2, result.output.constData());
            QVERIFY(result.output.contains("Persistence failure: " + point.chopped(7).toUtf8()));
            const bool committed = Committed(root.path());
            const auto recovered = Run(root.path(), "resume");
            QVERIFY2(recovered.code == 0, recovered.output.constData());
            QCOMPARE(Tree(root.path()), committed ? newFiles : oldFiles);
            if (point.startsWith("filesync:")) ++fileFailures; else ++directoryFailures;
        }
        QVERIFY(fileFailures > 0 && directoryFailures > 0);
        qInfo("nativeSyncFailuresAreFatal: PASS; file sync failures = %d; directory sync failures = %d", fileFailures, directoryFailures);
    }
    void identitiesVisibilityAndTraffic() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        Seed(root.path());
        const auto oldFiles = Tree(root.path());
        const auto expected = Expected(oldFiles);
        const auto result = Run(root.path(), "start");
        QVERIFY2(result.code == 0, result.output.constData());
        QCOMPARE(Tree(root.path()), expected);
        CurrentDirectory cwd(root.path());
        Globals globals;
        QVERIFY(!Renumber::Pending());
        QVERIFY(globals.store.Load());
        QVERIFY(globals.manager.LoadManager());
        QCOMPARE(globals.store.current_group, 1);
        QCOMPARE(globals.store.remember_id, 2);
        QCOMPARE(globals.manager.groupsTabOrder, QList<int>({0, 1, 2, 3}));
        QCOMPARE(int(globals.manager.profiles.size()), 5);
        QCOMPARE(int(globals.manager.groups.size()), 4);
        const QList<int> oldIds{41, 7, 90, 99, 120};
        for (int id = 0; id < oldIds.size(); ++id) {
            const auto profile = globals.manager.GetProfile(id);
            QVERIFY(profile != nullptr);
            QCOMPARE(profile->id, id);
            QCOMPARE(profile->traffic_data->id, id);
            QVERIFY(profile->traffic_data->tag.empty());
            QCOMPARE(profile->traffic_data->downlink, static_cast<long long>(1000 + oldIds.at(id)));
            QCOMPARE(profile->traffic_data->uplink, static_cast<long long>(2000 + oldIds.at(id)));
            QCOMPARE(profile->latency, oldIds.at(id));
            QCOMPARE(profile->full_test_report, QString("report-%1").arg(oldIds.at(id)));
            const auto raw = Object(Renumber::Read(profile->fn));
            QCOMPARE(raw.value("identity").toString(), QString("profile-%1").arg(oldIds.at(id)));
        }
        QCOMPARE(globals.manager.GetGroup(0)->front_proxy_id, 2);
        QCOMPARE(globals.manager.GetGroup(0)->order, QList<int>({0}));
        QCOMPARE(globals.manager.GetGroup(1)->order, QList<int>({1}));
        QVERIFY(globals.manager.GetGroup(1)->recoveryRequired);
        QVERIFY(globals.manager.GetGroup(2)->order.isEmpty());
        QCOMPARE(globals.manager.GetGroup(3)->order, QList<int>({4}));
        for (int gid = 0; gid < 4; ++gid) QVERIFY(globals.manager.GetGroup(gid)->order_explicit);
        QCOMPARE(globals.manager.GetGroup(1)->Profiles().size(), 2);
        QCOMPARE(globals.manager.GetGroup(1)->ProfilesWithOrder().size(), 1);
        QCOMPARE(globals.manager.GetGroup(2)->Profiles().size(), 1);
        QCOMPARE(globals.manager.GetGroup(2)->ProfilesWithOrder().size(), 0);
        QCOMPARE(Object(Renumber::Read("profiles/0.json")).value("bean").toObject().value("list").toArray(), QJsonArray({1, 2}));
        NekoGui_traffic::TrafficLooper looper;
        const auto profile = globals.manager.GetProfile(2);
        profile->traffic_data->tag = "new-tag";
        const auto session = looper.MakeSession({profile->traffic_data}, {});
        QCOMPARE(session->targets.size(), 1);
        QCOMPARE(session->targets.first().id, 2);
        QVERIFY(session->targets.first().profile.lock() == profile);
        QCOMPARE(Renumber::Read("groups/corrupt/evidence.corrupt"), oldFiles.value("groups/corrupt/evidence.corrupt"));
        QCOMPARE(Renumber::Read("profiles/notes.txt"), oldFiles.value("profiles/notes.txt"));
    }
    void overlappingFilenamesPreserveLogicalProfiles() {
        QTemporaryDir fixture;
        Seed(fixture.path());
        {
            CurrentDirectory cwd(fixture.path());
            // Old hidden profile 0 is overwritten by the renumbered chain,
            // while that hidden logical profile moves to destination 2.
            auto hidden = Object(Renumber::Read("profiles/90.json"));
            hidden["id"] = 0;
            Put("profiles/0.json", Json(hidden));
            QVERIFY(QFile::remove("profiles/90.json"));
            auto group = Object(Renumber::Read("groups/30.json"));
            group["front_proxy_id"] = 0;
            Put("groups/30.json", Json(group));
            auto chain = Object(Renumber::Read("profiles/41.json"));
            auto bean = chain.value("bean").toObject();
            bean["list"] = QJsonArray{7, 0};
            chain["bean"] = bean;
            Put("profiles/41.json", Json(chain));
            auto settings = Object(Renumber::Read("groups/nekobox.json"));
            settings["remember_id"] = 0;
            Put("groups/nekobox.json", Json(settings));
        }
        const auto oldFiles = Tree(fixture.path());
        const auto expected = Expected(oldFiles);
        QCOMPARE(Object(oldFiles.value("profiles/0.json")).value("identity").toString(), QString("profile-90"));
        QCOMPARE(Object(expected.value("profiles/0.json")).value("identity").toString(), QString("profile-41"));
        QCOMPARE(Object(expected.value("profiles/2.json")).value("identity").toString(), QString("profile-90"));
        QTemporaryDir baseline;
        CopyCheckpoint(fixture.path(), baseline.path());
        const auto trace = Run(baseline.path(), "start");
        QVERIFY2(trace.code == 0, trace.output.constData());
        for (const auto &point : {QString("rename:install:profiles/0.json:before"),
                                  QString("rename:install:profiles/0.json:after")}) {
            QTemporaryDir root;
            CopyCheckpoint(fixture.path(), root.path());
            QCOMPARE(Run(root.path(), "start", IndexOf(trace.points, point)).code, 86);
            const auto recovered = Run(root.path(), "resume");
            QVERIFY2(recovered.code == 0, recovered.output.constData());
            QCOMPARE(Tree(root.path()), expected);
        }
    }
    void rejectsEveryReferenceClassBeforeJournal() {
        for (int which = 0; which < 7; ++which) {
            QTemporaryDir root;
            Seed(root.path());
            CurrentDirectory cwd(root.path());
            const QStringList paths{"groups/pm.json", "groups/8.json", "groups/30.json", "profiles/41.json",
                                    "groups/nekobox.json", "groups/nekobox.json", "profiles/90.json"};
            const QStringList keys{"groups", "order", "front_proxy_id", "bean", "current_group", "remember_id", "gid"};
            const auto path = paths.at(which);
            auto object = Object(Renumber::Read(path));
            if (which == 0 || which == 1) object[keys.at(which)] = QJsonArray{999};
            else if (which == 3) {
                auto bean = object.value("bean").toObject(); bean["list"] = QJsonArray{999}; object["bean"] = bean;
            } else object[keys.at(which)] = 999;
            Put(path, Json(object));
            const auto before = Renumber::Scan();
            QVERIFY_EXCEPTION_THROWN(NekoGui::BuildRenumbered(before), Renumber::Error);
            QCOMPARE(Run(root.path(), "start").code, 2);
            QCOMPARE(Renumber::Scan(), before);
            QVERIFY(!Renumber::Pending());
            QVERIFY(QDir(".").entryList(QStringList{".renumber-build-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        }
    }
    void strictValidationAndFilenameCollision() {
        const QList<QByteArray> invalid{"{", "[]", "{\"id\":\"bad\"}"};
        for (const auto &bytes : invalid) {
            QTemporaryDir root;
            Seed(root.path());
            Put(root.path() + "/profiles/7.json", bytes);
            const auto before = Tree(root.path());
            QCOMPARE(Run(root.path(), "start").code, 2);
            QCOMPARE(Tree(root.path()), before);
            QVERIFY(!QFileInfo::exists(root.path() + "/.renumber"));
        }
        QTemporaryDir root;
        Seed(root.path());
        Put(root.path() + "/profiles/07.json", Renumber::Read(root.path() + "/profiles/7.json"));
        const auto before = Tree(root.path());
        QCOMPARE(Run(root.path(), "start").code, 2);
        QCOMPARE(Tree(root.path()), before);
    }
    void sentinelsAndLegacyOrders() {
        QTemporaryDir root;
        Seed(root.path());
        CurrentDirectory cwd(root.path());
        auto settings = Object(Renumber::Read("groups/nekobox.json"));
        settings["current_group"] = -1;
        for (int sentinel : {-1, -1919}) {
            settings["remember_id"] = sentinel;
            Put("groups/nekobox.json", Json(settings));
            const auto updated = Object(Expected(Renumber::Scan()).value("groups/nekobox.json"));
            QCOMPARE(updated.value("current_group").toInt(), -1);
            QCOMPARE(updated.value("remember_id").toInt(), sentinel);
        }
        auto group = Object(Renumber::Read("groups/8.json"));
        group.remove("order_explicit");
        group["order"] = QJsonArray{7};
        Put("groups/8.json", Json(group));
        const auto updated = Object(Expected(Renumber::Scan()).value("groups/1.json"));
        QVERIFY(updated.value("order_explicit").toBool());
        QCOMPARE(updated.value("order").toArray(), QJsonArray({1}));
    }
    void forwardCrashAndIoFailureMatrix() {
        QTemporaryDir baseline;
        Seed(baseline.path());
        const auto oldFiles = Tree(baseline.path());
        const auto newFiles = Expected(oldFiles);
        const auto trace = Run(baseline.path(), "start");
        QVERIFY2(trace.code == 0, trace.output.constData());
        QVERIFY(trace.points.size() > 30);
        int crashes = 0, failures = 0;
        for (int cut = 0; cut < trace.points.size(); ++cut) {
            QTemporaryDir root;
            Seed(root.path());
            QCOMPARE(Run(root.path(), "start", cut).code, 86);
            ++crashes;
            const bool committed = Committed(root.path());
            const auto recovered = Run(root.path(), "resume");
            QVERIFY2(recovered.code == 0, recovered.output.constData());
            QCOMPARE(Tree(root.path()), committed ? newFiles : oldFiles);
            QCOMPARE(Run(root.path(), "resume").code, 0);
            QCOMPARE(Tree(root.path()), committed ? newFiles : oldFiles);
        }
        for (int cut = 0; cut < trace.points.size(); ++cut) {
            if (!trace.points.at(cut).endsWith(":before")) continue;
            QTemporaryDir root;
            Seed(root.path());
            QCOMPARE(Run(root.path(), "start", cut, "fail").code, 2);
            ++failures;
            const bool committed = Committed(root.path());
            const auto recovered = Run(root.path(), "resume");
            QVERIFY2(recovered.code == 0, recovered.output.constData());
            QCOMPARE(Tree(root.path()), committed ? newFiles : oldFiles);
        }
        qInfo("forward crash matrix: PASS; abrupt exits = %d; I/O failures = %d", crashes, failures);
    }
    void recoveryCrashMatrix() {
        QTemporaryDir baseline;
        Seed(baseline.path());
        const auto oldFiles = Tree(baseline.path());
        const auto newFiles = Expected(oldFiles);
        const auto forward = Run(baseline.path(), "start");
        QVERIFY2(forward.code == 0, forward.output.constData());
        // Set NKR_RENUMBER_EXHAUSTIVE=1 to crash at EVERY recovery boundary,
        // starting from every state and every installed/removed checkpoint.
        const bool exhaustive = qEnvironmentVariableIntValue("NKR_RENUMBER_EXHAUSTIVE") != 0;
        QList<int> startingCuts;
        for (int i = 0; i < forward.points.size(); ++i) {
            const auto point = forward.points.at(i);
            if (point == "rename:publish:after" || point.startsWith("state:") ||
                point.startsWith("installed:") || point.startsWith("removed:")) startingCuts.append(i);
        }
        int abruptExits = 0, recoveryCuts = 0;
        for (int startCut : startingCuts) {
            QTemporaryDir probe;
            Seed(probe.path());
            QCOMPARE(Run(probe.path(), "start", startCut).code, 86);
            ++abruptExits;
            const bool committed = Committed(probe.path());
            QTemporaryDir checkpoint;
            CopyCheckpoint(probe.path(), checkpoint.path());
            const auto recoveryTrace = Run(probe.path(), "resume");
            QVERIFY2(recoveryTrace.code == 0, recoveryTrace.output.constData());
            for (int cut = 0; cut < recoveryTrace.points.size(); ++cut) {
                const auto point = recoveryTrace.points.at(cut);
                if (!exhaustive && point != "recovery:validated" && !point.startsWith("state:") &&
                    !point.startsWith("installed:") && !point.startsWith("removed:") &&
                    !point.startsWith("archived:")) continue;
                QTemporaryDir root;
                CopyCheckpoint(checkpoint.path(), root.path());
                QCOMPARE(Run(root.path(), "resume", cut).code, 86);
                ++abruptExits;
                ++recoveryCuts;
                const auto recovered = Run(root.path(), "resume");
                QVERIFY2(recovered.code == 0, recovered.output.constData());
                QCOMPARE(Tree(root.path()), committed ? newFiles : oldFiles);
            }
            qInfo("recovery checkpoint %s: PASS; cumulative recovery cuts = %d", qPrintable(forward.points.at(startCut)), recoveryCuts);
        }
        qInfo("recovery crash matrix: %s PASS; starting checkpoints = %d; recovery cuts = %d; abrupt-exit count = %d",
              exhaustive ? "EXHAUSTIVE" : "focused", int(startingCuts.size()), recoveryCuts, abruptExits);
    }
    void backupAndStagingCorruptionNeverCommits() {
        for (const auto &kind : {QString("corrupt-backup"), QString("corrupt-stage")}) {
            QTemporaryDir root;
            Seed(root.path());
            const auto oldFiles = Tree(root.path());
            QCOMPARE(Run(root.path(), "start", -1, kind).code, 2);
            QCOMPARE(Tree(root.path()), oldFiles);
            QVERIFY(!QFileInfo::exists(root.path() + "/.renumber/COMMIT"));
            QCOMPARE(Run(root.path(), "resume").code, 2);
            QCOMPARE(Tree(root.path()), oldFiles);
        }
    }
    void malformedJournalBlocksLoadingAndReload() {
        QTemporaryDir baseline;
        Seed(baseline.path());
        const auto trace = Run(baseline.path(), "start");
        const int staged = IndexOf(trace.points, "state:STAGED");
        for (int variant = 0; variant < 5; ++variant) {
            QTemporaryDir root;
            Seed(root.path());
            QCOMPARE(Run(root.path(), "start", staged).code, 86);
            CurrentDirectory cwd(root.path());
            const auto before = Renumber::Scan();
            if (variant == 0) Put(".renumber/bundle.json", "{");
            if (variant == 1) Put(".renumber/UNKNOWN", "unexpected");
            if (variant == 2) Put(".renumber/STAGED", "wrong digest\n");
            if (variant == 3) QVERIFY(QFile::remove(".renumber/BACKUP_DONE"));
            if (variant == 4) {
                auto envelope = Object(Renumber::Read(".renumber/bundle.json"));
                auto payload = Object(QByteArray::fromBase64(envelope.value("payload").toString().toLatin1()));
                payload["version"] = 999;
                const auto bytes = Json(payload);
                envelope["payload"] = QString::fromLatin1(bytes.toBase64());
                envelope["sha256"] = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                Put(".renumber/bundle.json", Json(envelope));
            }
            QCOMPARE(Run(root.path(), "resume").code, 2);
            QCOMPARE(Renumber::Scan(), before);
            Globals globals;
            globals.store.current_group = 777;
            globals.store.last_save_content = "cache";
            globals.manager.groupsTabOrder = {888};
            QVERIFY(!globals.store.Load());
            QVERIFY(!globals.store.LoadOrCreate());
            QVERIFY(!globals.manager.LoadManager());
            QVERIFY(!NekoGui::ReloadConfiguration());
            QCOMPARE(globals.store.current_group, 777);
            QCOMPARE(globals.store.last_save_content, QByteArray("cache"));
            QCOMPARE(globals.manager.groupsTabOrder, QList<int>({888}));
            QCOMPARE(Renumber::Scan(), before);
        }
    }
    void startupRecoversBeforeFirstLoad() {
        QTemporaryDir baseline;
        Seed(baseline.path());
        const auto trace = Run(baseline.path(), "start");
        const int committed = IndexOf(trace.points, "state:COMMIT");
        QTemporaryDir root;
        Seed(root.path());
        QCOMPARE(Run(root.path(), "start", committed).code, 86);
        CurrentDirectory cwd(root.path());
        Globals globals;
        globals.store.current_group = 777;
        QVERIFY(!globals.store.Load());
        QCOMPARE(globals.store.current_group, 777);
        Renumber::Lease lease;
        NekoGui::PrepareConfiguration(false, lease);
        QVERIFY(!Renumber::Pending());
        QVERIFY(globals.store.LoadOrCreate());
        QVERIFY(globals.manager.LoadManager());
        QCOMPARE(globals.store.current_group, 1);
        QCOMPARE(globals.store.remember_id, 2);
        QVERIFY(globals.manager.GetProfile(2) != nullptr);
    }
    void participatingInstancesExcludeMigration() {
        QTemporaryDir root;
        Seed(root.path());
        QProcess first;
        first.setWorkingDirectory(root.path());
        first.setProcessChannelMode(QProcess::MergedChannels);
        first.start(QCoreApplication::applicationFilePath(), {"--worker", "hold-normal", "-1", "crash"});
        QVERIFY(first.waitForStarted(10000));
        QVERIFY(WaitReady(first));
        QCOMPARE(Run(root.path(), "resume").code, 0);
        QCOMPARE(Run(root.path(), "start").code, 2);
        first.write("\n");
        QVERIFY(first.waitForFinished(10000));
        QCOMPARE(first.exitCode(), 0);
        QProcess exclusive;
        exclusive.setWorkingDirectory(root.path());
        exclusive.setProcessChannelMode(QProcess::MergedChannels);
        exclusive.start(QCoreApplication::applicationFilePath(), {"--worker", "hold-exclusive", "-1", "crash"});
        QVERIFY(exclusive.waitForStarted(10000));
        QVERIFY(WaitReady(exclusive));
        QCOMPARE(Run(root.path(), "resume").code, 2);
        exclusive.kill();
        QVERIFY(exclusive.waitForFinished(10000));
        QCOMPARE(Run(root.path(), "start").code, 0);
    }
};
int main(int argc, char **argv) {
    QApplication application(argc, argv);
    initializeUiThreadDispatcher(&application);
    const auto arguments = application.arguments();
    if (arguments.size() >= 5 && arguments.at(1) == "--worker") return Worker(arguments);
    RenumberTest test;
    const int result = QTest::qExec(&test, argc, argv);
    shutdownUiThreadDispatcher();
    return result;
}
#include "tst_renumber.moc"
