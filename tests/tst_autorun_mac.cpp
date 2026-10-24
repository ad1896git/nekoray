#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <vector>

#include "sys/AutoRun.hpp"
#include "sys/AutoRunMac_p.hpp"

using NkrAutoRunMac::Api;
using NkrAutoRunMac::Operation;

namespace {

QString appDirectory() {
    return QString::fromUtf8("/Applications/Neko 空格.app/Contents/MacOS");
}

QString appBundle() {
    return NkrAutoRunMac::bundlePath(appDirectory());
}

enum class Fault {
    None,
    Url,
    List,
    Snapshot,
    Resolve,
    Insert
};

class Fake {
public:
    Api api;
    Fault fault = Fault::None;
    int requests = 0;
    int insertCalls = 0;
    int removeCalls = 0;
    int failRemoveAt = 0;
    bool invalidRelease = false;
    QHash<quintptr, int> credits;
    QSet<quintptr> unresolved;
    std::vector<CFURLRef> entries;

    Fake() : api(NkrAutoRunMac::nativeApi()) {
        makeUrl_ = api.createUrl;

        api.createUrl = [this](const QByteArray &path) -> CFURLRef {
            ++requests;
            if (fault == Fault::Url) {
                return nullptr;
            }
            return own(makeUrl_(path));
        };

        api.createList = [this]() -> LSSharedFileListRef {
            ++requests;
            if (fault == Fault::List) {
                return nullptr;
            }
            auto list = CFArrayCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            return own(reinterpret_cast<LSSharedFileListRef>(list));
        };

        api.copySnapshot = [this](LSSharedFileListRef) -> CFArrayRef {
            ++requests;
            if (fault == Fault::Snapshot) {
                return nullptr;
            }

            std::vector<const void *> values;
            for (CFURLRef entry : entries) {
                values.push_back(entry);
            }
            return own(CFArrayCreate(
                kCFAllocatorDefault,
                values.empty() ? nullptr : values.data(),
                static_cast<CFIndex>(values.size()),
                &kCFTypeArrayCallBacks));
        };

        api.resolve = [this](LSSharedFileListItemRef item) -> CFURLRef {
            ++requests;
            if (fault == Fault::Resolve ||
                unresolved.contains(reinterpret_cast<quintptr>(item))) {
                return nullptr;
            }

            auto url = reinterpret_cast<CFURLRef>(item);
            return own(static_cast<CFURLRef>(CFRetain(url)));
        };

        api.insert = [this](LSSharedFileListRef,
                            CFURLRef url) -> LSSharedFileListItemRef {
            ++requests;
            ++insertCalls;
            if (fault == Fault::Insert) {
                return nullptr;
            }

            entries.push_back(static_cast<CFURLRef>(CFRetain(url)));
            auto item = reinterpret_cast<LSSharedFileListItemRef>(
                const_cast<void *>(CFRetain(url)));
            return own(item);
        };

        api.remove = [this](LSSharedFileListRef,
                            LSSharedFileListItemRef item) {
            ++requests;
            ++removeCalls;
            if (removeCalls == failRemoveAt) {
                return false;
            }

            auto url = reinterpret_cast<CFURLRef>(item);
            auto it = std::find(entries.begin(), entries.end(), url);
            if (it == entries.end()) {
                return false;
            }
            CFRelease(*it);
            entries.erase(it);
            return true;
        };

        api.release = [this](CFTypeRef value) {
            const auto key = reinterpret_cast<quintptr>(value);
            const int count = credits.value(key);
            if (!value || count <= 0) {
                invalidRelease = true;
                return;
            }
            if (count == 1) {
                credits.remove(key);
            } else {
                credits.insert(key, count - 1);
            }
            CFRelease(value);
        };
    }

    ~Fake() {
        for (CFURLRef entry : entries) {
            CFRelease(entry);
        }
    }

    Fake(const Fake &) = delete;
    Fake &operator=(const Fake &) = delete;

    CFURLRef add(const QString &path) {
        CFURLRef value = makeUrl_(QFile::encodeName(path));
        if (!value) {
            qFatal("Cannot allocate fake login-item URL");
        }
        entries.push_back(value);
        return value;
    }

    bool run(Operation operation) {
        return NkrAutoRunMac::execute(appDirectory(), operation, api);
    }

    bool balanced() const {
        return credits.isEmpty() && !invalidRelease;
    }

private:
    std::function<CFURLRef(const QByteArray &)> makeUrl_;

    template<typename T>
    T own(T value) {
        if (value) {
            const auto key = reinterpret_cast<quintptr>(value);
            credits.insert(key, credits.value(key) + 1);
        }
        return value;
    }
};

} // namespace

class AutoRunMacTest : public QObject {
    Q_OBJECT

private slots:
    void bundlePaths() {
        QCOMPARE(
            NkrAutoRunMac::bundlePath(appDirectory()),
            QString::fromUtf8("/Applications/Neko 空格.app"));

        QCOMPARE(
            NkrAutoRunMac::bundlePath(
                QStringLiteral("/Applications/A.app/Contents/./MacOS/")),
            QStringLiteral("/Applications/A.app"));

        const QStringList invalid{
            QStringLiteral("/tmp/build"),
            QStringLiteral("/Applications/A/Contents/MacOS"),
            QStringLiteral("/Applications/A.app/Other/MacOS"),
            QStringLiteral("/Applications/A.app/Contents"),
            QStringLiteral("/Applications/A.app"),
            QStringLiteral("A.app/Contents/MacOS"),
            QString()
        };

        for (const QString &path : invalid) {
            QVERIFY(NkrAutoRunMac::bundlePath(path).isEmpty());
            Fake fake;
            for (Operation operation :
                 {Operation::Query, Operation::Enable, Operation::Disable}) {
                QVERIFY(!NkrAutoRunMac::execute(path, operation, fake.api));
            }
            QCOMPARE(fake.requests, 0);
            QVERIFY(fake.balanced());
        }
    }

    void failures_data() {
        QTest::addColumn<int>("faultValue");
        QTest::addColumn<int>("operationValue");

        const Fault earlyFaults[]{
            Fault::Url, Fault::List, Fault::Snapshot, Fault::Resolve
        };
        const Operation operations[]{
            Operation::Query, Operation::Enable, Operation::Disable
        };

        for (Fault fault : earlyFaults) {
            for (Operation operation : operations) {
                const QByteArray name =
                    QByteArray::number(static_cast<int>(fault)) + "-" +
                    QByteArray::number(static_cast<int>(operation));
                QTest::newRow(name.constData())
                    << static_cast<int>(fault)
                    << static_cast<int>(operation);
            }
        }

        QTest::newRow("insert-failure")
            << static_cast<int>(Fault::Insert)
            << static_cast<int>(Operation::Enable);
    }

    void failures() {
        QFETCH(int, faultValue);
        QFETCH(int, operationValue);

        Fake fake;
        fake.fault = static_cast<Fault>(faultValue);
        if (fake.fault == Fault::Resolve) {
            fake.add(appBundle());
        }

        QVERIFY(!fake.run(static_cast<Operation>(operationValue)));
        QCOMPARE(fake.insertCalls, fake.fault == Fault::Insert ? 1 : 0);
        QCOMPARE(fake.removeCalls, 0);
        QVERIFY(fake.balanced());
    }

    void idempotenceAndUnrelatedItems() {
        Fake fake;
        CFURLRef other = fake.add(QStringLiteral("/Applications/Other.app"));

        QVERIFY(!fake.run(Operation::Query));
        QVERIFY(fake.run(Operation::Disable));
        QCOMPARE(fake.removeCalls, 0);

        for (int i = 0; i < 10; ++i) {
            QVERIFY(fake.run(Operation::Enable));
            QVERIFY(fake.run(Operation::Query));
            QVERIFY(fake.balanced());
        }

        QCOMPARE(fake.insertCalls, 1);
        QCOMPARE(fake.removeCalls, 0);
        QCOMPARE(static_cast<int>(fake.entries.size()), 2);

        for (int i = 0; i < 10; ++i) {
            QVERIFY(fake.run(Operation::Disable));
            QVERIFY(!fake.run(Operation::Query));
            QVERIFY(fake.balanced());
        }

        QCOMPARE(fake.removeCalls, 1);
        QCOMPARE(static_cast<int>(fake.entries.size()), 1);
        QVERIFY(fake.entries.front() == other);
    }

    void preexistingDuplicates() {
        Fake fake;
        fake.add(appBundle());
        CFURLRef other = fake.add(QStringLiteral("/Applications/Other.app"));
        fake.add(appBundle());
        fake.add(appBundle());

        QVERIFY(fake.run(Operation::Enable));
        QCOMPARE(fake.insertCalls, 0);
        QCOMPARE(fake.removeCalls, 0);
        QCOMPARE(static_cast<int>(fake.entries.size()), 4);

        QVERIFY(fake.run(Operation::Disable));
        QCOMPARE(fake.removeCalls, 3);
        QCOMPARE(static_cast<int>(fake.entries.size()), 1);
        QVERIFY(fake.entries.front() == other);
        QVERIFY(fake.balanced());
    }

    void unresolvedItemDoesNotMeanAbsent() {
        Fake fake;
        CFURLRef broken = fake.add(QStringLiteral("/Applications/Missing.app"));
        fake.unresolved.insert(reinterpret_cast<quintptr>(broken));

        QVERIFY(!fake.run(Operation::Query));
        QVERIFY(!fake.run(Operation::Enable));
        QCOMPARE(fake.insertCalls, 0);
        QVERIFY(fake.balanced());

        fake.add(appBundle());
        QVERIFY(fake.run(Operation::Query));
        QVERIFY(fake.run(Operation::Enable));
        QCOMPARE(fake.insertCalls, 0);

        // Remove the known match, but report incomplete processing.
        QVERIFY(!fake.run(Operation::Disable));
        QCOMPARE(fake.removeCalls, 1);
        QCOMPARE(static_cast<int>(fake.entries.size()), 1);
        QVERIFY(fake.entries.front() == broken);
        QVERIFY(fake.balanced());
    }

    void removalFailureDoesNotStopCleanup() {
        Fake fake;
        fake.add(appBundle());
        CFURLRef other = fake.add(QStringLiteral("/Applications/Other.app"));
        fake.add(appBundle());
        fake.add(appBundle());
        fake.failRemoveAt = 1;

        QVERIFY(!fake.run(Operation::Disable));
        QCOMPARE(fake.removeCalls, 3);
        QCOMPARE(static_cast<int>(fake.entries.size()), 2);
        QVERIFY(fake.balanced());

        fake.failRemoveAt = 0;
        QVERIFY(fake.run(Operation::Disable));
        QCOMPARE(fake.removeCalls, 4);
        QCOMPARE(static_cast<int>(fake.entries.size()), 1);
        QVERIFY(fake.entries.front() == other);
        QVERIFY(fake.balanced());
    }

    void publicBareExecutable() {
        QVERIFY(NkrAutoRunMac::bundlePath(
                    QCoreApplication::applicationDirPath()).isEmpty());

        const char *warning =
            "macOS autorun: unsupported executable layout; "
            "expected .app/Contents/MacOS";

        QTest::ignoreMessage(QtWarningMsg, warning);
        QVERIFY(!AutoRun_IsEnabled());

        QTest::ignoreMessage(QtWarningMsg, warning);
        AutoRun_SetEnabled(true);

        QTest::ignoreMessage(QtWarningMsg, warning);
        AutoRun_SetEnabled(false);
    }

    void nativeReadOnly() {
        if (qgetenv("NKR_W23_NATIVE_READ") != QByteArray("1")) {
            QSKIP("Opt-in native read-only smoke test");
        }

        const Api api = NkrAutoRunMac::nativeApi();
        LSSharedFileListRef list = api.createList();
        if (!list) {
            QSKIP("Native login-item list unavailable; record this limitation");
        }

        CFArrayRef snapshot = api.copySnapshot(list);
        if (!snapshot) {
            api.release(list);
            QSKIP("Native snapshot unavailable; record this limitation");
        }

        CFIndex resolved = 0;
        const CFIndex count = CFArrayGetCount(snapshot);
        for (CFIndex i = 0; i < count; ++i) {
            const void *value = CFArrayGetValueAtIndex(snapshot, i);
            if (!value) {
                continue;
            }

            auto item = static_cast<LSSharedFileListItemRef>(
                const_cast<void *>(value));
            CFURLRef url = api.resolve(item);
            if (url) {
                ++resolved;
                api.release(url);
            }
        }

        api.release(snapshot);
        api.release(list);

        qInfo() << "Native login-item snapshot read:"
                << static_cast<qlonglong>(count)
                << "items,"
                << static_cast<qlonglong>(resolved)
                << "resolved; no mutations";
    }
};

QTEST_GUILESS_MAIN(AutoRunMacTest)
#include "tst_autorun_mac.moc"
