#include <QtTest/QtTest>

#include <QList>
#include <QStringList>

#include <map>
#include <memory>

#include "ui/StopTrafficPersistence.hpp"

namespace {

struct TestTrafficItem {
    int id;
};

struct TestProfile {
    int id;
    QStringList *events;
    bool changed;

    bool Save() {
        events->append(QStringLiteral("save:%1").arg(id));
        return changed;
    }
};

} // namespace

class StopTrafficPersistenceTest : public QObject {
    Q_OBJECT

private slots:
    void savesSurvivors_data();
    void savesSurvivors();
};

void StopTrafficPersistenceTest::savesSurvivors_data() {
    QTest::addColumn<QList<int>>("itemIds");
    QTest::addColumn<QList<int>>("deletedIds");
    QTest::addColumn<bool>("saveChanged");
    QTest::addColumn<QStringList>("expectedEvents");

    QTest::newRow("all-present")
        << QList<int>{1, 2, 3}
        << QList<int>{}
        << true
        << QStringList{
               "save:1", "refresh:1",
               "save:2", "refresh:2",
               "save:3", "refresh:3"};

    QTest::newRow("deleted-middle-child")
        << QList<int>{1, 2, 3}
        << QList<int>{2}
        << true
        << QStringList{
               "save:1", "refresh:1",
               "save:3", "refresh:3"};

    QTest::newRow("deleted-first-child")
        << QList<int>{1, 2, 3}
        << QList<int>{1}
        << true
        << QStringList{
               "save:2", "refresh:2",
               "save:3", "refresh:3"};

    QTest::newRow("deleted-last-child")
        << QList<int>{1, 2, 3}
        << QList<int>{3}
        << true
        << QStringList{
               "save:1", "refresh:1",
               "save:2", "refresh:2"};

    QTest::newRow("deleted-child-group")
        << QList<int>{1, 2, 3}
        << QList<int>{2, 3}
        << true
        << QStringList{"save:1", "refresh:1"};

    QTest::newRow("all-items-missing")
        << QList<int>{1, 2, 3}
        << QList<int>{1, 2, 3}
        << true
        << QStringList{};

    QTest::newRow("empty-items")
        << QList<int>{}
        << QList<int>{}
        << true
        << QStringList{};

    QTest::newRow("missing-negative-id")
        << QList<int>{-1, 1}
        << QList<int>{-1}
        << true
        << QStringList{"save:1", "refresh:1"};

    QTest::newRow("save-reports-unchanged")
        << QList<int>{1, 2, 3}
        << QList<int>{2}
        << false
        << QStringList{
               "save:1", "refresh:1",
               "save:3", "refresh:3"};
}

void StopTrafficPersistenceTest::savesSurvivors() {
    QFETCH(QList<int>, itemIds);
    QFETCH(QList<int>, deletedIds);
    QFETCH(bool, saveChanged);
    QFETCH(QStringList, expectedEvents);

    QStringList events;
    QList<int> lookedUp;
    QList<std::shared_ptr<TestTrafficItem>> items;
    std::map<int, std::shared_ptr<TestProfile>> profiles;

    for (int id: itemIds) {
        items.append(std::make_shared<TestTrafficItem>(
            TestTrafficItem{id}));
        profiles.emplace(
            id,
            std::make_shared<TestProfile>(
                TestProfile{id, &events, saveChanged}));
    }

    for (int id: deletedIds) {
        profiles.erase(id);
    }

    const auto survivorCount = profiles.size();

    NekoGui::SaveExistingTrafficProfiles(
        items,
        [&](int id) -> std::shared_ptr<TestProfile> {
            lookedUp.append(id);
            const auto found = profiles.find(id);
            if (found == profiles.end()) {
                return {};
            }
            return found->second;
        },
        [&](int id) {
            events.append(QStringLiteral("refresh:%1").arg(id));
        });

    QCOMPARE(lookedUp, itemIds);
    QCOMPARE(events, expectedEvents);
    QVERIFY(profiles.size() == survivorCount);

    for (int id: deletedIds) {
        QVERIFY(profiles.find(id) == profiles.end());
    }
}

QTEST_APPLESS_MAIN(StopTrafficPersistenceTest)

#include "tst_stop_traffic_persistence.moc"
