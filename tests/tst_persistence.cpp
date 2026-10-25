#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <memory>

#include "db/Database.hpp"
#include "fmt/SocksHttpBean.hpp"

using namespace NekoGui;

namespace {

bool Put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(bytes) != bytes.size()) return false;
    return file.flush();
}

QByteArray Read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QStringList IsolatedFiles(const QString &parent) {
    return QDir(parent + "/corrupt").entryList(
        QStringList() << "*.corrupt", QDir::Files, QDir::Name);
}

QByteArray Json(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

class Probe : public JsonStore {
public:
    int number = 1;
    QString text = "old";
    bool enabled = false;
    QList<int> ids = {1, 2};
    QList<QString> names = {"a", "b"};
    long long counter = 4;

    Probe() {
        _add(new configItem("number", &number, itemType::integer));
        _add(new configItem("text", &text, itemType::string));
        _add(new configItem("enabled", &enabled, itemType::boolean));
        _add(new configItem("ids", &ids, itemType::integerList));
        _add(new configItem("names", &names, itemType::stringList));
        _add(new configItem("counter", &counter, itemType::integer64));
    }
};

template<class Base>
class FaultStore : public Base {
public:
    using Base::Base;

    enum class Fault {
        None,
        ShortWrite,
        Commit
    };

    Fault fault = Fault::None;

protected:
    qint64 WriteSaveFile(QSaveFile &file,
                        const QByteArray &bytes) override {
        if (fault == Fault::ShortWrite) {
            const QByteArray prefix = bytes.left(bytes.size() / 2);
            return file.write(prefix);
        }
        return Base::WriteSaveFile(file, bytes);
    }

    bool CommitSaveFile(QSaveFile &file) override {
        if (fault == Fault::Commit) return false;
        return Base::CommitSaveFile(file);
    }
};

} // namespace

class PersistenceTest : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> temporary;
    QString previousDirectory;
    ProfileManager *previousManager = nullptr;
    DataStore *previousStore = nullptr;
    std::unique_ptr<ProfileManager> manager;
    std::unique_ptr<DataStore> store;

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
        profileManager = previousManager;
        dataStore = previousStore;
        manager.reset();
        store.reset();

        QVERIFY(QDir::setCurrent(previousDirectory));
        temporary.reset();
    }

    void saveResultsAndCache() {
        FaultStore<Probe> value;
        value.fn = "groups/probe.json";

        auto result = value.SaveChecked();
        QVERIFY(result.ok);
        QVERIFY(result.changed);
        const QByteArray original = Read(value.fn);
        QCOMPARE(value.last_save_content, original);

        result = value.SaveChecked();
        QVERIFY(result.ok);
        QVERIFY(!result.changed);
        QVERIFY(!value.Save());

        value.text = "new";
        QVERIFY(value.Save());
        QVERIFY(!value.Save());

        value.save_control_no_save = true;
        value.text = "suppressed";
        const QByteArray saved = Read(value.fn);
        const QByteArray cache = value.last_save_content;
        result = value.SaveChecked();
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(Read(value.fn), saved);
        QCOMPARE(value.last_save_content, cache);
        value.save_control_no_save = false;

        // An unchanged cache must not bypass actual persistence.
        value.text = "new";
        QVERIFY(QFile::remove(value.fn));
        result = value.SaveChecked();
        QVERIFY(result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(Read(value.fn), cache);
    }

    void failedWriteKeepsOldFile_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("short-write") << 1;
        QTest::newRow("commit") << 2;
    }

    void failedWriteKeepsOldFile() {
        QFETCH(int, fault);

        FaultStore<Probe> value;
        value.fn = "groups/probe.json";
        QVERIFY(value.SaveChecked().ok);

        const QByteArray original = Read(value.fn);
        const QByteArray cache = value.last_save_content;

        value.text = "new";
        value.fault = fault == 1
            ? FaultStore<Probe>::Fault::ShortWrite
            : FaultStore<Probe>::Fault::Commit;

        const auto result = value.SaveChecked();
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(Read(value.fn), original);
        QCOMPARE(value.last_save_content, cache);

        QJsonParseError error;
        const auto document =
            QJsonDocument::fromJson(Read(value.fn), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());

        value.fault = FaultStore<Probe>::Fault::None;
        const auto retry = value.SaveChecked();
        QVERIFY(retry.ok);
        QVERIFY(retry.changed);
        QCOMPARE(value.last_save_content, Read(value.fn));
    }

    void openFailureIsNotSuccess() {
        Probe value;
        value.fn = "groups/probe.json";
        QVERIFY(value.SaveChecked().ok);
        const QByteArray cache = value.last_save_content;

        QVERIFY(Put("blocked", "regular file"));
        value.fn = "blocked/config.json";
        value.text = "new";

        const auto result = value.SaveChecked();
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(value.last_save_content, cache);
        QCOMPARE(Read("blocked"), QByteArray("regular file"));
    }

    void invalidBytesDoNotMutate_data() {
        QTest::addColumn<QByteArray>("bytes");

        QTest::newRow("truncated") << QByteArray("{\"");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("null") << QByteArray("null");
        QTest::newRow("integer-string")
            << QByteArray("{\"number\":\"2\",\"text\":\"changed\"}");
        QTest::newRow("integer-fraction")
            << QByteArray("{\"number\":1.5}");
        QTest::newRow("integer-overflow")
            << QByteArray("{\"number\":2147483648}");
        QTest::newRow("boolean-number")
            << QByteArray("{\"enabled\":1}");
        QTest::newRow("integer-list-element")
            << QByteArray("{\"ids\":[1,\"2\"]}");
        QTest::newRow("string-list-element")
            << QByteArray("{\"names\":[\"a\",2]}");
        QTest::newRow("integer64-overflow")
            << QByteArray("{\"counter\":9223372036854775808}");
    }

    void invalidBytesDoNotMutate() {
        QFETCH(QByteArray, bytes);

        Probe value;
        value.last_save_content = "cache";
        const QByteArray before = value.ToJsonBytes();
        int callbacks = 0;
        value.callback_after_load = [&] { ++callbacks; };

        QVERIFY(!value.FromJsonBytes(bytes));
        QCOMPARE(value.ToJsonBytes(), before);
        QCOMPARE(value.last_save_content, QByteArray("cache"));
        QCOMPARE(callbacks, 0);
    }

    void validBytesAndUnknownKeys() {
        Probe value;
        int callbacks = 0;
        value.callback_after_load = [&] { ++callbacks; };

        QVERIFY(value.FromJsonBytes(
            "{\"number\":3,\"text\":\"ok\",\"unknown\":{\"x\":1}}"));
        QCOMPARE(value.number, 3);
        QCOMPARE(value.text, QString("ok"));
        QCOMPARE(callbacks, 1);
    }

    void profileRoundTrips_data() {
        QTest::addColumn<QString>("type");
        const QStringList types = {
            "socks", "http", "shadowsocks", "chain", "vmess",
            "trojan", "vless", "naive", "hysteria2", "tuic", "custom"
        };
        for (const auto &type : types) {
            QTest::newRow(qPrintable(type)) << type;
        }
    }

    void profileRoundTrips() {
        QFETCH(QString, type);

        auto group = ProfileManager::NewGroup();
        group->name = "group";
        QVERIFY(manager->AddGroup(group));

        auto profile = ProfileManager::NewProxyEntity(type);
        profile->traffic_data->uplink = 123;
        profile->traffic_data->downlink = 456;
        QVERIFY(manager->AddProfile(profile, group->id));

        const int id = profile->id;
        const QByteArray bytes = Read(profile->fn);
        QVERIFY(manager->LoadManager());

        const auto loaded = manager->GetProfile(id);
        QVERIFY(loaded);
        QCOMPARE(loaded->type, type);
        QCOMPARE(loaded->ToJsonBytes(), bytes);
        QCOMPARE(loaded->traffic_data->uplink, 123LL);
        QCOMPARE(loaded->traffic_data->downlink, 456LL);
    }

    void invalidProfileIsIsolated_data() {
        QTest::addColumn<QByteArray>("bytes");

        auto profile = ProfileManager::NewProxyEntity("socks");
        profile->id = 7;
        profile->gid = 0;
        const QJsonObject valid = profile->ToJson();

        QTest::newRow("truncated") << QByteArray("{\"");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("bad-id") << QByteArray("{\"id\":\"x\"}");

        auto object = valid;
        object.insert("type", "unknown-protocol");
        QTest::newRow("unknown-type") << Json(object);

        object = valid;
        object.remove("bean");
        QTest::newRow("missing-bean") << Json(object);

        object = valid;
        object.insert("id", 8);
        QTest::newRow("filename-id-mismatch") << Json(object);

        object = valid;
        auto bean = object.value("bean").toObject();
        bean.insert("port", "1080");
        object.insert("bean", bean);
        QTest::newRow("bean-wrong-type") << Json(object);

        object = valid;
        bean = object.value("bean").toObject();
        bean.remove("_v");
        object.insert("bean", bean);
        QTest::newRow("missing-bean-version") << Json(object);

        object = valid;
        bean = object.value("bean").toObject();
        bean.insert("_v", -114514);
        object.insert("bean", bean);
        QTest::newRow("invalid-bean-version") << Json(object);

        object = valid;
        bean = object.value("bean").toObject();
        auto stream = bean.value("stream").toObject();
        stream.insert("net", 42);
        bean.insert("stream", stream);
        object.insert("bean", bean);
        QTest::newRow("nested-wrong-type") << Json(object);
    }

    void invalidProfileIsIsolated() {
        QFETCH(QByteArray, bytes);

        auto group = ProfileManager::NewGroup();
        group->name = "group";
        QVERIFY(manager->AddGroup(group));
        QVERIFY(Put("profiles/7.json", bytes));

        QVERIFY(manager->LoadManager());
        QVERIFY(!manager->GetProfile(7));
        QVERIFY(!QFileInfo::exists("profiles/7.json"));

        const auto isolated = IsolatedFiles("profiles");
        QCOMPARE(isolated.size(), 1);
        QCOMPARE(Read("profiles/corrupt/" + isolated.first()), bytes);

        QVERIFY(manager->LoadManager());
        QVERIFY(!manager->GetProfile(7));
        QCOMPARE(IsolatedFiles("profiles").size(), 1);
        QVERIFY(manager->GetGroup(group->id)->ProfilesWithOrder().isEmpty());
    }

    void invalidGroupAndManagerAreIsolated() {
        auto group = ProfileManager::NewGroup();
        group->name = "good";
        QVERIFY(manager->AddGroup(group));

        QVERIFY(Put("groups/7.json", "{\"id\":7,\"name\":42}"));
        QVERIFY(Put("groups/pm.json", "{\"groups\":[\"bad\"]}"));

        QVERIFY(manager->LoadManager());
        QVERIFY(manager->GetGroup(0));
        QVERIFY(!manager->GetGroup(7));
        QCOMPARE(manager->groupsTabOrder, QList<int>({0}));
        QCOMPARE(IsolatedFiles("groups").size(), 2);

        QVERIFY(manager->LoadManager());
        QVERIFY(!manager->GetGroup(7));
        QCOMPARE(IsolatedFiles("groups").size(), 2);
    }

    void groupNameSchema() {
        Group group;
        QVERIFY(!group.FromJsonBytes("{\"id\":0}"));
        QVERIFY(!group.FromJsonBytes("{\"id\":0,\"name\":1}"));
        QVERIFY(group.FromJsonBytes("{\"id\":0,\"name\":\"\"}"));

        auto emptyName = ProfileManager::NewGroup();
        QVERIFY(manager->AddGroup(emptyName));
        QVERIFY(manager->LoadManager());
        QVERIFY(manager->GetGroup(emptyName->id));
        QCOMPARE(manager->GetGroup(emptyName->id)->name, QString());
    }

    void datastoreDefaultsPreserveCorruptBytes() {
        const QByteArray invalid("{\"");
        QVERIFY(Put("groups/nekobox.json", invalid));

        DataStore settings;
        settings.fn = "groups/nekobox.json";
        QVERIFY(settings.LoadOrCreate());
        QVERIFY(!settings.IsolatedPath().isEmpty());
        QVERIFY(QFileInfo::exists(settings.IsolatedPath()));

        const auto isolated = IsolatedFiles("groups");
        QCOMPARE(isolated.size(), 1);
        QCOMPARE(Read("groups/corrupt/" + isolated.first()), invalid);

        QJsonParseError error;
        const auto document =
            QJsonDocument::fromJson(Read(settings.fn), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        QVERIFY(settings.Load());
    }

    void isolationFailureBlocksOverwriteAndRetryWorks() {
        const QByteArray invalid("{\"");
        QVERIFY(Put("groups/nekobox.json", invalid));
        QVERIFY(Put("groups/corrupt", "blocks directory creation"));

        DataStore settings;
        settings.fn = "groups/nekobox.json";
        const QByteArray before = settings.ToJsonBytes();
        settings.last_save_content = "previous-cache";

        QVERIFY(!settings.LoadOrCreate());
        QCOMPARE(settings.ToJsonBytes(), before);
        QCOMPARE(settings.last_save_content, QByteArray("previous-cache"));
        QCOMPARE(Read(settings.fn), invalid);
        QVERIFY(!settings.SaveChecked().ok);
        QCOMPARE(Read(settings.fn), invalid);

        QVERIFY(QFile::remove("groups/corrupt"));
        QVERIFY(settings.LoadOrCreate());
        QCOMPARE(IsolatedFiles("groups").size(), 1);
    }

    void unreadableShapeIsNotIsolatedOrOverwritten() {
        QVERIFY(QDir().mkdir("groups/nekobox.json"));

        DataStore settings;
        settings.fn = "groups/nekobox.json";
        QVERIFY(!settings.LoadOrCreate());
        QVERIFY(!settings.SaveChecked().ok);
        QVERIFY(QFileInfo(settings.fn).isDir());
        QVERIFY(IsolatedFiles("groups").isEmpty());
    }

    void addFailuresRestoreIdentityAndContainers() {
        auto group = std::make_shared<FaultStore<Group>>();
        group->name = "group";
        group->fn = "original-group-path";
        group->last_save_content = "group-cache";
        group->fault = FaultStore<Group>::Fault::Commit;

        QVERIFY(!manager->AddGroup(group));
        QCOMPARE(group->id, -1);
        QCOMPARE(group->fn, QString("original-group-path"));
        QCOMPARE(group->last_save_content, QByteArray("group-cache"));
        QVERIFY(manager->groups.empty());
        QVERIFY(manager->groupsTabOrder.isEmpty());
        QVERIFY(!QFileInfo::exists("groups/0.json"));

        group->fault = FaultStore<Group>::Fault::None;
        QVERIFY(manager->AddGroup(group));
        QCOMPARE(group->id, 0);
        QCOMPARE(manager->groupsTabOrder, QList<int>({0}));
        QVERIFY(!manager->AddGroup(group));
        QCOMPARE(manager->groups.size(), std::size_t(1));

        auto profile = std::make_shared<FaultStore<ProxyEntity>>(
            new NekoGui_fmt::SocksHttpBean(
                NekoGui_fmt::SocksHttpBean::type_Socks5),
            QString("socks"));
        profile->gid = 77;
        profile->fn = "original-profile-path";
        profile->last_save_content = "profile-cache";
        profile->fault = FaultStore<ProxyEntity>::Fault::Commit;

        const auto groupOrder = group->order;
        QVERIFY(!manager->AddProfile(profile, group->id));
        QCOMPARE(profile->id, -1);
        QCOMPARE(profile->gid, 77);
        QCOMPARE(profile->fn, QString("original-profile-path"));
        QCOMPARE(profile->last_save_content, QByteArray("profile-cache"));
        QVERIFY(manager->profiles.empty());
        QCOMPARE(group->order, groupOrder);
        QVERIFY(!QFileInfo::exists("profiles/0.json"));

        profile->fault = FaultStore<ProxyEntity>::Fault::None;
        QVERIFY(manager->AddProfile(profile, group->id));
        QCOMPARE(profile->id, 0);
        QCOMPARE(profile->gid, group->id);
        QCOMPARE(group->order, groupOrder);
        QVERIFY(!manager->AddProfile(profile, group->id));
        QCOMPARE(manager->profiles.size(), std::size_t(1));
    }

    void occupiedFilesAreNotOverwritten() {
        QVERIFY(Put("profiles/0.json", "unloaded profile"));
        QVERIFY(Put("groups/0.json", "unloaded group"));

        auto group = ProfileManager::NewGroup();
        group->name = "new";
        QVERIFY(manager->AddGroup(group));
        QCOMPARE(group->id, 1);
        QCOMPARE(Read("groups/0.json"), QByteArray("unloaded group"));

        auto profile = ProfileManager::NewProxyEntity("socks");
        QVERIFY(manager->AddProfile(profile, group->id));
        QCOMPARE(profile->id, 1);
        QCOMPARE(Read("profiles/0.json"), QByteArray("unloaded profile"));
    }

    void deletionFailureKeepsRegistration() {
        auto group = ProfileManager::NewGroup();
        group->name = "group";
        QVERIFY(manager->AddGroup(group));

        auto profile = ProfileManager::NewProxyEntity("socks");
        QVERIFY(manager->AddProfile(profile, group->id));
        const int id = profile->id;
        const QByteArray bytes = Read(profile->fn);

        dataStore->started_id = id;
        QVERIFY(!manager->DeleteProfile(id));
        QVERIFY(manager->GetProfile(id));
        QCOMPARE(Read(profile->fn), bytes);
        dataStore->started_id = -1;

        QVERIFY(QFile::rename(profile->fn, profile->fn + ".backup"));
        QVERIFY(QDir().mkdir(profile->fn));
        QVERIFY(!manager->DeleteProfile(id));
        QCOMPARE(manager->GetProfile(id).get(), profile.get());
        QVERIFY(QFileInfo(profile->fn).isDir());

        QVERIFY(QDir().rmdir(profile->fn));
        QVERIFY(QFile::rename(profile->fn + ".backup", profile->fn));
        QVERIFY(manager->DeleteProfile(id));
        QVERIFY(!manager->GetProfile(id));
        QVERIFY(!QFileInfo::exists(profile->fn));
        QVERIFY(!manager->DeleteProfile(id));
        QVERIFY(!manager->DeleteProfile(-1));
    }

    void groupDeletionKeepsParentOnChildFailure() {
        auto first = ProfileManager::NewGroup();
        first->name = "first";
        QVERIFY(manager->AddGroup(first));

        auto second = ProfileManager::NewGroup();
        second->name = "second";
        QVERIFY(manager->AddGroup(second));

        auto profile = ProfileManager::NewProxyEntity("socks");
        QVERIFY(manager->AddProfile(profile, first->id));
        dataStore->started_id = profile->id;

        manager->DeleteGroup(first->id);
        QVERIFY(manager->GetGroup(first->id));
        QVERIFY(manager->GetProfile(profile->id));
        QVERIFY(QFileInfo::exists(first->fn));
    }

    void failedManagerReloadKeepsPreviousContainers() {
        auto group = ProfileManager::NewGroup();
        group->name = "group";
        QVERIFY(manager->AddGroup(group));

        auto profile = ProfileManager::NewProxyEntity("socks");
        QVERIFY(manager->AddProfile(profile, group->id));

        QVERIFY(QDir().mkdir("groups/pm.json"));
        QVERIFY(!manager->LoadManager());

        QCOMPARE(manager->GetGroup(group->id).get(), group.get());
        QCOMPARE(manager->GetProfile(profile->id).get(), profile.get());
        QCOMPARE(manager->groupsTabOrder, QList<int>({group->id}));
    }

    void defaultGroupFailureIsReported() {
        QVERIFY(QDir().rename("groups", "groups-original"));
        QVERIFY(Put("groups", "not a directory"));

        QVERIFY(!manager->LoadManager());
        QVERIFY(manager->groups.empty());
        QVERIFY(manager->groupsTabOrder.isEmpty());
        QCOMPARE(Read("groups"), QByteArray("not a directory"));
    }

    void fullFilesystem() {
        const QString root =
            QString::fromLocal8Bit(qgetenv("NKR_TEST_FULL_DIR"));
        if (root.isEmpty()) {
            QSKIP("Requires the separate Linux tmpfs acceptance command.");
        }

        QTemporaryDir area(root + "/persistence-XXXXXX");
        QVERIFY(area.isValid());

        Probe value;
        value.fn = area.path() + "/config.json";
        QVERIFY(value.SaveChecked().ok);
        const QByteArray original = Read(value.fn);
        const QByteArray cache = value.last_save_content;

        QFile filler(area.path() + "/fill");
        QVERIFY(filler.open(QIODevice::WriteOnly));
        const QByteArray block(4096, 'x');
        bool reachedLimit = false;
        for (int i = 0; i < 4096; ++i) {
            if (filler.write(block) != block.size()) {
                reachedLimit = true;
                break;
            }
        }
        if (!filler.flush()) reachedLimit = true;
        QVERIFY2(reachedLimit, "The supplied filesystem was not filled");

        value.text = QString(2 * 1024 * 1024, QLatin1Char('y'));
        const auto result = value.SaveChecked();
        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(Read(value.fn), original);
        QCOMPARE(value.last_save_content, cache);

        filler.close();
        QVERIFY(filler.remove());
    }

    void readOnlyDirectory() {
        if (qgetenv("NKR_TEST_READONLY") != "1") {
            QSKIP("Requires the separate non-root permission acceptance command.");
        }

        QVERIFY(QDir().mkdir("readonly"));

        Probe value;
        value.fn = "readonly/config.json";
        QVERIFY(value.SaveChecked().ok);
        const QByteArray original = Read(value.fn);
        const QByteArray cache = value.last_save_content;

        const QFileDevice::Permissions writable =
            QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner;
        const QFileDevice::Permissions readonly =
            QFileDevice::ReadOwner |
            QFileDevice::ExeOwner;

        QVERIFY(QFile::setPermissions("readonly", readonly));
        value.text = "changed";
        const auto result = value.SaveChecked();

        // Restore permissions before assertions so test cleanup can succeed.
        QVERIFY(QFile::setPermissions("readonly", writable));

        QVERIFY(!result.ok);
        QVERIFY(!result.changed);
        QCOMPARE(Read(value.fn), original);
        QCOMPARE(value.last_save_content, cache);
    }
};

QTEST_MAIN(PersistenceTest)
#include "tst_persistence.moc"
