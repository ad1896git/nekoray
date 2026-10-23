#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QTemporaryDir>
#include <memory>
#include "db/Database.hpp"

using namespace NekoGui;
namespace {
QMap<QString, QByteArray> Files(const QString &root) {
    QMap<QString, QByteArray> result;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read test fixture file");
        result.insert(QDir(root).relativeFilePath(path), file.readAll());
    }
    return result;
}
QByteArray JsonBytes(JsonStore &store) {
    return QJsonDocument(store.ToJson()).toJson(QJsonDocument::Compact);
}
}

class MoveProfileTest : public QObject {
    Q_OBJECT
    QString previousDirectory;
    ProfileManager *previousManager = nullptr;
    DataStore *previousStore = nullptr;
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<ProfileManager> manager;
    std::unique_ptr<DataStore> store;
private slots:
    void init() {
        previousDirectory = QDir::currentPath();
        previousManager = profileManager;
        previousStore = dataStore;
        temporary.reset(new QTemporaryDir);
        QVERIFY(temporary->isValid());
        QVERIFY(QDir::setCurrent(temporary->path()));
        QVERIFY(QDir().mkdir("profiles"));
        QVERIFY(QDir().mkdir("groups"));
        manager.reset(new ProfileManager);
        store.reset(new DataStore);
        profileManager = manager.get();
        dataStore = store.get();
        dataStore->started_id = -1;
        dataStore->flag_reorder = false;
    }
    void cleanup() {
        profileManager = previousManager;
        dataStore = previousStore;
        manager.reset();
        store.reset();
        QVERIFY(QDir::setCurrent(previousDirectory));
        temporary.reset();
    }
    void move_data() {
        QTest::addColumn<int>("oldOrderMode");
        QTest::addColumn<bool>("targetExplicit");
        QTest::addColumn<QString>("destination");
        for (int mode = 0; mode < 3; ++mode) {
            for (bool explicitTarget : {false, true}) {
                for (const QString &destination : QStringList{"missing", "deleted", "negative", "same", "valid"}) {
                    const QByteArray name = QString("%1-%2-%3").arg(mode).arg(explicitTarget).arg(destination).toUtf8();
                    QTest::newRow(name.constData()) << mode << explicitTarget << destination;
                }
            }
        }
    }
    void move() {
        QFETCH(int, oldOrderMode);
        QFETCH(bool, targetExplicit);
        QFETCH(QString, destination);
        auto oldGroup = ProfileManager::NewGroup();
        oldGroup->name = "source";
        QVERIFY(manager->AddGroup(oldGroup));
        auto target = ProfileManager::NewGroup();
        target->name = "target";
        target->order_explicit = targetExplicit;
        QVERIFY(manager->AddGroup(target));
        auto profile = ProfileManager::NewProxyEntity("socks");
        QVERIFY(manager->AddProfile(profile, oldGroup->id));
        const int profileId = profile->id;
        if (oldOrderMode != 0) oldGroup->order = {profileId};
        oldGroup->order_explicit = oldOrderMode == 1;
        QVERIFY(oldGroup->SaveChecked().ok);
        int destinationId = target->id;
        if (destination == "missing") destinationId = target->id + 100;
        if (destination == "negative") destinationId = -1;
        if (destination == "same") destinationId = oldGroup->id;
        if (destination == "deleted") {
            manager->DeleteGroup(target->id);
            QVERIFY(!manager->GetGroup(target->id));
            QVERIFY(!QFileInfo::exists(target->fn));
        }
        const QByteArray profileBefore = JsonBytes(*profile);
        const QByteArray oldBefore = JsonBytes(*oldGroup);
        const QByteArray targetBefore = JsonBytes(*target);
        const QByteArray profileCache = profile->last_save_content;
        const QByteArray oldCache = oldGroup->last_save_content;
        const QByteArray targetCache = target->last_save_content;
        const auto filesBefore = Files(temporary->path());
        const auto tabsBefore = manager->groupsTabOrder;
        const int groupCount = static_cast<int>(manager->groups.size());
        manager->MoveProfile(profile, destinationId);
        QCOMPARE(profile->id, profileId);
        QCOMPARE(static_cast<int>(manager->profiles.size()), 1);
        QCOMPARE(static_cast<int>(manager->groups.size()), groupCount);
        QCOMPARE(manager->groupsTabOrder, tabsBefore);
        QVERIFY(manager->GetProfile(profileId) == profile);
        if (destination != "valid") {
            QCOMPARE(JsonBytes(*profile), profileBefore);
            QCOMPARE(JsonBytes(*oldGroup), oldBefore);
            QCOMPARE(JsonBytes(*target), targetBefore);
            QCOMPARE(profile->last_save_content, profileCache);
            QCOMPARE(oldGroup->last_save_content, oldCache);
            QCOMPARE(target->last_save_content, targetCache);
            QCOMPARE(Files(temporary->path()), filesBefore);
            QCOMPARE(profile->gid, oldGroup->id);
            QVERIFY(oldGroup->ProfilesWithOrder().contains(profile));
            return;
        }
        QCOMPARE(profile->gid, target->id);
        QVERIFY(!oldGroup->ProfilesWithOrder().contains(profile));
        QVERIFY(target->ProfilesWithOrder().contains(profile));
        QVERIFY(oldGroup->order.isEmpty());
        QCOMPARE(oldGroup->order_explicit, oldOrderMode != 0);
        QCOMPARE(target->order_explicit, targetExplicit);
        QCOMPARE(target->order, targetExplicit ? QList<int>{profileId} : QList<int>{});
        QJsonObject expected = QJsonDocument::fromJson(profileBefore).object();
        expected.insert("gid", target->id);
        QCOMPARE(profile->ToJson(), expected);
        const auto filesAfter = Files(temporary->path());
        manager->MoveProfile(profile, target->id);
        QCOMPARE(Files(temporary->path()), filesAfter);
        manager->SaveManager();
        QVERIFY(manager->LoadManager());
        auto loaded = manager->GetProfile(profileId);
        QVERIFY(loaded);
        QCOMPARE(loaded->gid, target->id);
        QVERIFY(manager->GetGroup(target->id)->ProfilesWithOrder().contains(loaded));
        QVERIFY(!manager->GetGroup(oldGroup->id)->ProfilesWithOrder().contains(loaded));
    }
    void nullProfileIsNoOp() {
        const auto before = Files(temporary->path());
        manager->MoveProfile({}, 123);
        QCOMPARE(Files(temporary->path()), before);
        QVERIFY(manager->profiles.empty());
        QVERIFY(manager->groups.empty());
    }
};
QTEST_MAIN(MoveProfileTest)
#include "tst_w19.moc"
