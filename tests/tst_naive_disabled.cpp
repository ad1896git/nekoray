#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QTemporaryDir>

#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "fmt/ChainBean.hpp"
#include "fmt/NaiveBean.hpp"
#include "sub/SubscriptionCommit.hpp"
#include "ui/edit/dialog_edit_profile.h"

using namespace NekoGui;

class NaiveDisabledTest : public QObject {
    Q_OBJECT
    QString previousDirectory;
    ProfileManager *previousManager = nullptr;
    DataStore *previousStore = nullptr;
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<ProfileManager> manager;
    std::unique_ptr<DataStore> store;

    static QByteArray read(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    static std::shared_ptr<ProxyEntity> naive() {
        auto ent = ProfileManager::NewProxyEntity("naive");
        auto bean = ent->NaiveBean();
        bean->name = "legacy naive";
        bean->serverAddress = "naive.example";
        bean->serverPort = 443;
        bean->username = "F22_USER";
        bean->password = "F22_PASSWORD";
        bean->extra_headers = "Authorization: F22_HEADER";
        bean->sni = "sni.example";
        bean->certificate = "F22_CERTIFICATE";
        bean->insecure_concurrency = 2;
        bean->disable_log = true;
        return ent;
    }

private slots:
    void init() {
        previousDirectory = QDir::currentPath();
        temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(temporary->isValid());
        QVERIFY(QDir::setCurrent(temporary->path()));
        QVERIFY(QDir().mkdir("profiles"));
        QVERIFY(QDir().mkdir("groups"));
        previousManager = profileManager;
        previousStore = dataStore;
        manager = std::make_unique<ProfileManager>();
        store = std::make_unique<DataStore>();
        profileManager = manager.get();
        dataStore = store.get();
        store->routing = std::make_unique<Routing>();
        store->current_group = 0;
        store->started_id = -1;
        store->flag_reorder = false;
        QVERIFY(manager->AddGroup(ProfileManager::NewGroup()));
    }

    void cleanup() {
        profileManager = previousManager;
        dataStore = previousStore;
        manager.reset();
        store.reset();
        QVERIFY(QDir::setCurrent(previousDirectory));
        temporary.reset();
    }

    void externalBuilderNeverProducesLaunchInputs_data() {
        QTest::addColumn<int>("externalStat");
        QTest::newRow("mapping") << 1;
        QTest::newRow("direct") << 2;
    }

    void externalBuilderNeverProducesLaunchInputs() {
        QFETCH(int, externalStat);
        store->extraCore->Set("naive", "/unused/naive");
        auto ent = naive();
        const auto before = ent->ToJsonBytes();
        const auto result = ent->bean->BuildExternal(20001, 20002, externalStat);
        // This assertion fails on base: --proxy= contains F22_USER:F22_PASSWORD.
        QVERIFY2(result.arguments.isEmpty(), "Naive must never produce command-line arguments");
        QCOMPARE(result.error, QString("Naive is disabled in this build"));
        QVERIFY(result.program.isEmpty());
        QVERIFY(result.env.isEmpty());
        QVERIFY(result.config_export.isEmpty());
        QVERIFY(!result.tempFiles);
        QVERIFY(!QFileInfo::exists("temp"));
        QCOMPARE(ent->ToJsonBytes(), before);
    }

    void creationIsBlocked() {
        auto ent = naive();
        const auto before = ent->ToJsonBytes();
        QVERIFY(!manager->AddProfile(ent, 0));
        QCOMPARE(ent->ToJsonBytes(), before);
        QVERIFY(ent->fn.isEmpty());
        QVERIFY(manager->profiles.empty());
        QVERIFY(QDir("profiles").entryList(QDir::Files).isEmpty());

        DialogEditProfile dialog("socks", 0);
        const auto types = dialog.findChild<QComboBox *>("type");
        QVERIFY(types);
        QVERIFY(types->findData("socks") >= 0);
        QCOMPARE(types->findData("naive"), -1);
    }

    void importsAndClonesCannotCreateProfiles() {
        namespace S = NekoGui_sub::Subscription;
        const auto ent = naive();
        const QStringList links{
            ent->bean->ToShareLink(),
            ent->bean->ToNekorayShareLink("naive") // also used by Clone
        };
        const auto group = manager->GetGroup(0);
        const auto before = read(group->fn);
        for (const auto &link : links) {
            S::Entities staged;
            if (S::Parse(link, staged)) {
                QVERIFY(!S::Succeeded(S::Commit(group, staged, false, false, "")));
            }
            QVERIFY(manager->profiles.empty());
            QCOMPARE(read(group->fn), before);
            QVERIFY(QDir("profiles").entryList(QDir::Files).isEmpty());
        }
    }

    void existingProfileRemainsIntact_data() {
        QTest::addColumn<bool>("vpn");
        QTest::addColumn<bool>("coreConfigured");
        QTest::addColumn<bool>("chained");
        for (bool vpn : {false, true}) {
            for (bool core : {false, true}) {
                for (bool chain : {false, true}) {
                    const auto name = QString("vpn-%1-core-%2-chain-%3").arg(vpn).arg(core).arg(chain);
                    QTest::newRow(qPrintable(name)) << vpn << core << chain;
                }
            }
        }
    }

    void existingProfileRemainsIntact() {
        QFETCH(bool, vpn);
        QFETCH(bool, coreConfigured);
        QFETCH(bool, chained);
        store->spmode_vpn = vpn;
        store->extraCore->Set("naive", coreConfigured ? "/unused/naive" : "");
        // Write a pre-existing profile without going through the new-profile API.
        auto ent = naive();
        ent->id = 0;
        ent->gid = 0;
        ent->fn = "profiles/0.json";
        QVERIFY(ent->SaveChecked().ok);
        const auto bytes = read(ent->fn);
        QVERIFY(!bytes.isEmpty());
        const auto json = ent->ToJsonBytes();
        QVERIFY(manager->LoadManager());
        ent = manager->GetProfile(0);
        QVERIFY(ent);
        QCOMPARE(ent->ToJsonBytes(), json);

        auto startEnt = ent;
        if (chained) {
            auto socks = ProfileManager::NewProxyEntity("socks");
            QVERIFY(manager->AddProfile(socks, 0));
            auto chain = ProfileManager::NewProxyEntity("chain");
            chain->ChainBean()->list = {socks->id, ent->id};
            QVERIFY(manager->AddProfile(chain, 0));
            startEnt = chain;
        }
        for (int mode = 0; mode < 3; ++mode) { // start, latency test, export
            const auto result = BuildConfig(startEnt, mode == 1, mode == 2);
            QCOMPARE(result->error, QString("Naive is disabled in this build"));
            QVERIFY(result->extRs.empty());
            QVERIFY(!QFileInfo::exists("temp"));
            QCOMPARE(ent->ToJsonBytes(), json);
            QCOMPARE(read(ent->fn), bytes);
        }
        QVERIFY(ent->SaveChecked().ok);
        QCOMPARE(read(ent->fn), bytes);
        QVERIFY(manager->LoadManager());
        const auto reloaded = manager->GetProfile(0);
        QVERIFY(reloaded);
        QCOMPARE(reloaded->ToJsonBytes(), json);
    }
};

QTEST_MAIN(NaiveDisabledTest)
#include "tst_naive_disabled.moc"
