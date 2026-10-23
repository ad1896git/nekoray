#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
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
#include "fmt/VMessBean.hpp"
#include "db/Renumber.hpp"
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
QString VMessJsonLink(const QByteArray &json) {
    return QStringLiteral("vmess://") + QString::fromLatin1(json.toBase64());
}
QJsonObject ValidVMessJson() {
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("add"), QStringLiteral("vmess.example")},
        {QStringLiteral("port"), 443}
    };
}
QMap<QString, QByteArray> VMessSavedFiles() {
    QMap<QString, QByteArray> result;
    const QStringList directories{QStringLiteral("profiles"), QStringLiteral("groups")};
    for (const QString &directory : directories) {
        const QDir dir(directory);
        const QStringList files = dir.entryList(QDir::Files, QDir::Name);
        for (const QString &file : files) {
            const QString path = dir.filePath(file);
            result.insert(path, Read(path));
        }
    }
    return result;
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
    void vmessRequiredFields_data() {
        QTest::addColumn<QString>("link");
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<int>("expectedPort");

        const auto addObject = [](const QByteArray &name,
                                  const QJsonObject &object,
                                  bool accepted,
                                  int expectedPort) {
            QTest::newRow(name.constData())
                << VMessJsonLink(QJsonDocument(object).toJson(QJsonDocument::Compact))
                << accepted << expectedPort;
        };
        struct PortCase {
            const char *name;
            QJsonValue value;
            bool accepted;
            int expectedPort;
        };
        const PortCase ports[] = {
            {"number-1", QJsonValue(1), true, 1},
            {"number-443", QJsonValue(443), true, 443},
            {"number-65535", QJsonValue(65535), true, 65535},
            {"string-1", QJsonValue(QStringLiteral("1")), true, 1},
            {"string-443", QJsonValue(QStringLiteral("443")), true, 443},
            {"string-65535", QJsonValue(QStringLiteral("65535")), true, 65535},
            {"string-trimmed", QJsonValue(QStringLiteral(" 443 ")), true, 443},
            {"string-tabs", QJsonValue(QStringLiteral("\t443\r\n")), true, 443},
            {"string-leading-zeroes", QJsonValue(QStringLiteral("00443")), true, 443},
            {"number-zero", QJsonValue(0), false, 0},
            {"number-negative", QJsonValue(-1), false, 0},
            {"number-overflow", QJsonValue(65536), false, 0},
            {"number-fraction", QJsonValue(443.5), false, 0},
            {"number-below-minimum", QJsonValue(0.5), false, 0},
            {"number-above-maximum", QJsonValue(65535.5), false, 0},
            {"number-huge", QJsonValue(1e100), false, 0},
            {"string-zero", QJsonValue(QStringLiteral("0")), false, 0},
            {"string-negative", QJsonValue(QStringLiteral("-1")), false, 0},
            {"string-overflow", QJsonValue(QStringLiteral("65536")), false, 0},
            {"string-huge", QJsonValue(QStringLiteral("999999999999999999999999999999")), false, 0},
            {"string-empty", QJsonValue(QStringLiteral("")), false, 0},
            {"string-blank", QJsonValue(QStringLiteral(" \t ")), false, 0},
            {"string-bad", QJsonValue(QStringLiteral("bad")), false, 0},
            {"string-fraction", QJsonValue(QStringLiteral("443.5")), false, 0},
            {"string-decimal-point", QJsonValue(QStringLiteral("443.0")), false, 0},
            {"string-exponent", QJsonValue(QStringLiteral("4.43e2")), false, 0},
            {"string-plus", QJsonValue(QStringLiteral("+443")), false, 0},
            {"string-hex", QJsonValue(QStringLiteral("0x1bb")), false, 0},
            {"string-internal-space", QJsonValue(QStringLiteral("4 43")), false, 0},
            {"string-trailing-junk", QJsonValue(QStringLiteral("443x")), false, 0},
            {"string-non-ascii", QJsonValue(QString::fromUtf8("\xEF\xBC\x94\xEF\xBC\x94\xEF\xBC\x93")), false, 0},
            {"boolean-true", QJsonValue(true), false, 0},
            {"boolean-false", QJsonValue(false), false, 0},
            {"null", QJsonValue(QJsonValue::Null), false, 0},
            {"array", QJsonValue(QJsonArray{}), false, 0},
            {"object", QJsonValue(QJsonObject{}), false, 0}
        };
        for (const auto &test : ports) {
            auto object = ValidVMessJson();
            object.insert(QStringLiteral("port"), test.value);
            addObject(QByteArray("port-") + test.name, object, test.accepted, test.expectedPort);
        }
        auto missingPort = ValidVMessJson();
        missingPort.remove(QStringLiteral("port"));
        addObject("port-missing", missingPort, false, 0);

        struct TextCase { const char *name; QJsonValue value; };
        const TextCase invalidText[] = {
            {"empty", QJsonValue(QStringLiteral(""))},
            {"blank", QJsonValue(QStringLiteral(" \t\r\n"))},
            {"number", QJsonValue(123)},
            {"boolean", QJsonValue(true)},
            {"null", QJsonValue(QJsonValue::Null)},
            {"array", QJsonValue(QJsonArray{})},
            {"object", QJsonValue(QJsonObject{})}
        };
        const QStringList requiredText{QStringLiteral("id"), QStringLiteral("add")};
        for (const QString &field : requiredText) {
            auto missing = ValidVMessJson();
            missing.remove(field);
            addObject(field.toUtf8() + "-missing", missing, false, 0);
            for (const auto &test : invalidText) {
                auto object = ValidVMessJson();
                object.insert(field, test.value);
                addObject(field.toUtf8() + "-" + test.name, object, false, 0);
            }
        }
        auto namedId = ValidVMessJson();
        namedId.insert(QStringLiteral("id"), QStringLiteral("named-user"));
        addObject("noncanonical-id-remains-supported", namedId, true, 443);
        auto paddedText = ValidVMessJson();
        paddedText.insert(QStringLiteral("id"), QStringLiteral(" user "));
        paddedText.insert(QStringLiteral("add"), QStringLiteral(" vmess.example "));
        addObject("required-text-preserves-whitespace", paddedText, true, 443);
        addObject("optional-fields-only",
                  QJsonObject{{QStringLiteral("ps"), QStringLiteral("not-a-node")}}, false, 0);
        const auto addRaw = [](const char *name, const QByteArray &json, bool accepted, int port) {
            QTest::newRow(name) << VMessJsonLink(json) << accepted << port;
        };
        addRaw("numeric-integral-decimal", R"({"id":"user","add":"vmess.example","port":443.0})", true, 443);
        addRaw("numeric-integral-exponent", R"({"id":"user","add":"vmess.example","port":4.43e2})", true, 443);
        addRaw("empty-object", "{}", false, 0);
        addRaw("top-level-array", "[1,2]", false, 0);
        addRaw("top-level-null", "null", false, 0);
        addRaw("malformed-json", "{\"id\":", false, 0);
        addRaw("non-json", "not json", false, 0);
        QTest::newRow("invalid-base64") << QStringLiteral("vmess://%%%") << false << 0;
        struct UriCase { const char *name; const char *uri; bool accepted; int expectedPort; };
        const UriCase uris[] = {
            {"uri-default-port", "vmess://11111111-1111-4111-8111-111111111111@vmess.example", true, 443},
            {"uri-port-1", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:1", true, 1},
            {"uri-port-65535", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:65535", true, 65535},
            {"uri-port-zero", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:0", false, 0},
            {"uri-port-overflow", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:65536", false, 0},
            {"uri-port-negative", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:-1", false, 0},
            {"uri-port-fraction", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:443.5", false, 0},
            {"uri-port-junk", "vmess://11111111-1111-4111-8111-111111111111@vmess.example:bad", false, 0},
            {"uri-missing-id", "vmess://vmess.example:443", false, 0},
            {"uri-empty-id", "vmess://@vmess.example:443", false, 0},
            {"uri-blank-id", "vmess://%20%20@vmess.example:443", false, 0},
            {"uri-missing-host", "vmess://11111111-1111-4111-8111-111111111111@:443", false, 0}
        };
        for (const auto &test : uris) {
            QTest::newRow(test.name) << QString::fromLatin1(test.uri) << test.accepted << test.expectedPort;
        }
    }

    void vmessRequiredFields() {
        QFETCH(QString, link);
        QFETCH(bool, accepted);
        QFETCH(int, expectedPort);
        seed(0);
        const auto memoryBefore = group->ToJsonBytes();
        const auto filesBefore = VMessSavedFiles();
        const auto idsBefore = Ids(group->Profiles());
        NekoGui_fmt::VMessBean bean;
        QCOMPARE(bean.TryParseLink(link), accepted);
        S::Entities staged;
        QCOMPARE(S::Parse(link, staged), accepted);
        QCOMPARE(int(staged.size()), accepted ? 1 : 0);
        QCOMPARE(group->ToJsonBytes(), memoryBefore);
        QCOMPARE(VMessSavedFiles(), filesBefore);
        QCOMPARE(Ids(group->Profiles()), idsBefore);
        QCOMPARE(int(manager->profiles.size()), 2);
        if (accepted) {
            QCOMPARE(bean.serverPort, expectedPort);
            QVERIFY(!bean.uuid.trimmed().isEmpty());
            QVERIFY(!bean.serverAddress.trimmed().isEmpty());
            const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(
                link.mid(QStringLiteral("vmess://").size()).toLatin1()));
            if (document.isObject()) {
                QCOMPARE(bean.uuid, document.object().value(QStringLiteral("id")).toString());
                QCOMPARE(bean.serverAddress, document.object().value(QStringLiteral("add")).toString());
            }
            const auto &entity = staged.first();
            QCOMPARE(entity->type, QStringLiteral("vmess"));
            QCOMPARE(entity->id, -1);
            QVERIFY(entity->fn.isEmpty());
            QCOMPARE(entity->VMessBean()->uuid, bean.uuid);
            QCOMPARE(entity->bean->serverAddress, bean.serverAddress);
            QCOMPARE(entity->bean->serverPort, expectedPort);
            return;
        }
        for (const bool subscription : {false, true}) {
            for (const bool clear : {false, true}) {
                const auto reply = S::Apply(group, link, subscription, clear, "new-info");
                QCOMPARE(int(reply.status), int(S::Status::Rejected));
                QCOMPARE(group->ToJsonBytes(), memoryBefore);
                QCOMPARE(VMessSavedFiles(), filesBefore);
                QCOMPARE(Ids(group->Profiles()), idsBefore);
                QCOMPARE(int(manager->profiles.size()), 2);
                QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "b"}));
            }
        }
        reload();
        QCOMPARE(group->ToJsonBytes(), memoryBefore);
        QCOMPARE(VMessSavedFiles(), filesBefore);
        QCOMPARE(Ids(group->Profiles()), idsBefore);
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"a", "b"}));
    }

    void vmessMixedSubscription_data() {
        QTest::addColumn<bool>("subscription");
        QTest::addColumn<bool>("clear");
        QTest::addColumn<bool>("outerBase64");
        for (const bool subscription : {false, true}) {
            for (const bool clear : {false, true}) {
                for (const bool outerBase64 : {false, true}) {
                    const QByteArray name = QStringLiteral("subscription%1-clear%2-base64%3")
                        .arg(subscription ? 1 : 0).arg(clear ? 1 : 0).arg(outerBase64 ? 1 : 0).toLatin1();
                    QTest::newRow(name.constData()) << subscription << clear << outerBase64;
                }
            }
        }
    }
    void vmessMixedSubscription() {
        QFETCH(bool, subscription);
        QFETCH(bool, clear);
        QFETCH(bool, outerBase64);
        seed(0);
        const auto memoryBefore = group->ToJsonBytes();
        const auto filesBefore = VMessSavedFiles();
        const auto encode = [](const QJsonObject &object) {
            return VMessJsonLink(QJsonDocument(object).toJson(QJsonDocument::Compact));
        };
        auto valid = ValidVMessJson();
        valid.insert(QStringLiteral("ps"), QStringLiteral("json-valid"));
        valid.insert(QStringLiteral("port"), QStringLiteral(" 443 "));
        auto missingId = valid;
        missingId.remove(QStringLiteral("id"));
        auto fractional = valid;
        fractional.insert(QStringLiteral("port"), 443.5);
        auto missingAddress = valid;
        missingAddress.remove(QStringLiteral("add"));
        QString content = QStringList{
            encode(missingId), encode(valid), encode(fractional),
            QStringLiteral("vmess://11111111-1111-4111-8111-111111111111@vmess.example:8443?security=tls#uri-valid"),
            QStringLiteral("vmess://11111111-1111-4111-8111-111111111111@vmess.example:0"),
            encode(missingAddress)
        }.join(QStringLiteral("\n"));
        if (outerBase64) content = QString::fromLatin1(content.toUtf8().toBase64());
        S::Entities staged;
        QVERIFY(S::Parse(content, staged));
        QCOMPARE(int(staged.size()), 2);
        QCOMPARE(Names(staged), QStringList({"json-valid", "uri-valid"}));
        QCOMPARE(staged.at(0)->bean->serverPort, 443);
        QCOMPARE(staged.at(1)->bean->serverPort, 8443);
        for (const auto &entity : staged) {
            QCOMPARE(entity->type, QStringLiteral("vmess"));
            QCOMPARE(entity->id, -1);
            QVERIFY(entity->fn.isEmpty());
        }
        QCOMPARE(group->ToJsonBytes(), memoryBefore);
        QCOMPARE(VMessSavedFiles(), filesBefore);
        QCOMPARE(int(manager->profiles.size()), 2);
        const auto reply = S::Apply(group, content, subscription, clear, "new-info");
        QCOMPARE(int(reply.status), int(S::Status::Committed));
        const QStringList expected = subscription ? QStringList{"json-valid", "uri-valid"}
            : QStringList{"a", "b", "json-valid", "uri-valid"};
        QCOMPARE(Names(group->ProfilesWithOrder()), expected);
        QCOMPARE(int(manager->profiles.size()), int(expected.size()));
        QCOMPARE(int(QDir("profiles").entryList(QDir::Files).size()), int(expected.size()));
        QVERIFY(Unique(group->order));
        reload();
        QCOMPARE(Names(group->ProfilesWithOrder()), expected);
        QCOMPARE(int(manager->profiles.size()), int(expected.size()));
        QVERIFY(!group->recoveryRequired);
    }

    void vmessLegacyRoundTrip() {
        dataStore->old_share_link_format = true;
        NekoGui_fmt::VMessBean source;
        source.uuid = QStringLiteral("11111111-1111-4111-8111-111111111111");
        source.serverAddress = QStringLiteral("vmess.example");
        source.serverPort = 443;
        source.name = QStringLiteral("legacy fixture");
        source.aid = 64;
        source.security = QStringLiteral("aes-128-gcm");
        source.stream->network = QStringLiteral("ws");
        source.stream->host = QStringLiteral("cdn.example");
        source.stream->path = QStringLiteral("/socket");
        source.stream->sni = QStringLiteral("tls.example");
        source.stream->security = QStringLiteral("tls");
        source.stream->header_type = QStringLiteral("none");
        const QString link = source.ToShareLink();
        const QString prefix = QStringLiteral("vmess://");
        QVERIFY(link.startsWith(prefix));
        const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(link.mid(prefix.size()).toLatin1()));
        QVERIFY(document.isObject());
        const auto object = document.object();
        QVERIFY(object.value(QStringLiteral("port")).isString());
        QCOMPARE(object.value(QStringLiteral("port")).toString(), QStringLiteral("443"));
        NekoGui_fmt::VMessBean parsed;
        QVERIFY(parsed.TryParseLink(link));
        QCOMPARE(parsed.uuid, source.uuid);
        QCOMPARE(parsed.serverAddress, source.serverAddress);
        QCOMPARE(parsed.serverPort, source.serverPort);
        QCOMPARE(parsed.name, source.name);
        QCOMPARE(parsed.aid, source.aid);
        QCOMPARE(parsed.security, source.security);
        QCOMPARE(parsed.stream->network, source.stream->network);
        QCOMPARE(parsed.stream->host, source.stream->host);
        QCOMPARE(parsed.stream->path, source.stream->path);
        QCOMPARE(parsed.stream->sni, source.stream->sni);
        QCOMPARE(parsed.stream->security, source.stream->security);
        QCOMPARE(parsed.stream->header_type, source.stream->header_type);
        auto h2 = object;
        h2.insert(QStringLiteral("net"), QStringLiteral("h2"));
        NekoGui_fmt::VMessBean h2Parsed;
        QVERIFY(h2Parsed.TryParseLink(VMessJsonLink(QJsonDocument(h2).toJson(QJsonDocument::Compact))));
        QCOMPARE(h2Parsed.stream->network, QStringLiteral("http"));
        NekoGui_fmt::VMessBean minimal;
        QVERIFY(minimal.TryParseLink(VMessJsonLink(QJsonDocument(ValidVMessJson()).toJson(QJsonDocument::Compact))));
        QCOMPARE(minimal.aid, 0);
        QCOMPARE(minimal.security, QStringLiteral("auto"));
        QCOMPARE(minimal.stream->network, QStringLiteral("tcp"));
        QVERIFY(minimal.name.isEmpty());
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
        // Exercise startup's transaction before allowing either loader.
        dataStore->fn = "groups/nekobox.json";
        QVERIFY(dataStore->SaveChecked().ok);
        Renumber::Lease lease;
        NekoGui::PrepareConfiguration(true, lease);
        QVERIFY(!Renumber::Pending());
        QVERIFY(dataStore->Load());
        QVERIFY(manager->LoadManager());
        group = manager->GetGroup(0);
        QVERIFY(bool(group));
        QCOMPARE(Names(group->Profiles()), QStringList({"visible", "hidden"}));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"visible"}));
        QCOMPARE(int(manager->profiles.size()), 2);
        QVERIFY(group->order_explicit);
        QVERIFY(group->recoveryRequired);
        for (const auto &ent : group->Profiles()) QVERIFY(QFile::exists(ent->fn));
        reload();
        QCOMPARE(Names(group->Profiles()), QStringList({"visible", "hidden"}));
        QCOMPARE(Names(group->ProfilesWithOrder()), QStringList({"visible"}));
        QCOMPARE(int(manager->profiles.size()), 2);
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
