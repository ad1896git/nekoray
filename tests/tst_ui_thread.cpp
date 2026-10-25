#include "main/NekoGui_UiThread.hpp"
#include "main/NekoGui_UiThread_p.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QProcess>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using Result = UiThreadResult;
using Dispatcher = NekoGui_detail::UiThreadDispatcher;
using Task = NekoGui_detail::UiTask;
using WaitHooks = NekoGui_detail::UiWaitHooks;

static void acquireOrAbort(QSemaphore &semaphore) {
    if (!semaphore.tryAcquire(1, 5000)) {
        qFatal("test synchronization timed out");
    }
}

class ManualQueue {
    std::mutex mutex_;
    Task task_;
    QSemaphore posted_{0};

public:
    bool post(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        Q_ASSERT(!task_);
        task_ = std::move(task);
        posted_.release();
        return true;
    }

    Task take() {
        acquireOrAbort(posted_);
        std::lock_guard<std::mutex> lock(mutex_);
        Task task = std::move(task_);
        task_ = {};
        return task;
    }

    int queued() const {
        return posted_.available();
    }
};

class UiThreadTests : public QObject {
    Q_OBJECT

private slots:
    void callbackResult_data() {
        QTest::addColumn<int>("expected");
        QTest::newRow("committed")
            << static_cast<int>(Result::Committed);
        QTest::newRow("declined")
            << static_cast<int>(Result::NotCommitted);
        QTest::newRow("failed")
            << static_cast<int>(Result::Failed);
    }

    void callbackResult() {
        QFETCH(int, expected);
        ManualQueue queue;
        auto *uiThread = QThread::currentThread();
        Dispatcher dispatcher(
            uiThread, [&](Task task) { return queue.post(std::move(task)); });

        bool correctThread = false;
        Result actual = Result::Failed;
        std::thread worker([&] {
            actual = dispatcher.call(
                [&] {
                    correctThread = QThread::currentThread() == uiThread;
                    return static_cast<Result>(expected);
                },
                5000, 1000);
        });

        queue.take()();
        worker.join();

        QVERIFY(correctThread);
        QCOMPARE(static_cast<int>(actual), expected);
    }

    void callbackException_data() {
        QTest::addColumn<bool>("nonStandard");
        QTest::newRow("std-exception") << false;
        QTest::newRow("non-std-exception") << true;
    }

    void callbackException() {
        QFETCH(bool, nonStandard);
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        Result actual = Result::Committed;
        std::thread worker([&] {
            actual = dispatcher.call(
                [nonStandard]() -> Result {
                    if (nonStandard) {
                        throw 7;
                    }
                    throw std::runtime_error("test");
                },
                5000, 1000);
        });

        queue.take()();
        worker.join();

        QVERIFY(actual == Result::Failed);
    }

    void timeoutBeforeStartPreventsLateCommit() {
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        const auto counter = std::make_shared<int>(0);
        Result actual = Result::Committed;
        std::thread worker([&] {
            actual = dispatcher.call(
                [counter] {
                    ++*counter;
                    return Result::Committed;
                },
                0, 1000);
        });

        // The fake UI queue is deliberately not run until after the
        // caller has returned.
        worker.join();
        QVERIFY(actual == Result::NotCommitted);
        QCOMPARE(*counter, 0);

        queue.take()();
        QCOMPARE(*counter, 0);
    }

    void eventLoopNeverRuns() {
        const auto counter = std::make_shared<int>(0);
        Result actual = Result::Committed;

        {
            ManualQueue queue;
            Dispatcher dispatcher(
                QThread::currentThread(),
                [&](Task task) { return queue.post(std::move(task)); });

            std::thread worker([&] {
                actual = dispatcher.call(
                    [counter] {
                        ++*counter;
                        return Result::Committed;
                    },
                    0, 1000);
            });
            worker.join();

            QCOMPARE(queue.queued(), 1);
            // Destroy the queue without ever executing its callback.
        }

        QVERIFY(actual == Result::NotCommitted);
        QCOMPARE(*counter, 0);
    }

    void timeoutWhileRunning_data() {
        QTest::addColumn<bool>("shutdownDuringRun");
        QTest::newRow("running") << false;
        QTest::newRow("running-during-shutdown") << true;
    }

    void timeoutWhileRunning() {
        QFETCH(bool, shutdownDuringRun);
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        QSemaphore entered{0};
        QSemaphore cancellationLost{0};
        bool observedLostCancellation = false;
        const auto counter = std::make_shared<int>(0);

        WaitHooks hooks;
        hooks.firstWait = [&](QSemaphore &, int) {
            // Force the timeout decision only after Pending -> Running.
            acquireOrAbort(entered);
            return false;
        };
        hooks.cancelLost = [&] {
            observedLostCancellation = true;
            cancellationLost.release();
        };

        Result actual = Result::NotCommitted;
        std::thread worker([&] {
            actual = dispatcher.call(
                [&] {
                    if (shutdownDuringRun) {
                        dispatcher.stop();
                    }
                    entered.release();

                    // Keep the callback Running until the actual
                    // cancellation CAS has failed in production code.
                    acquireOrAbort(cancellationLost);
                    ++*counter;
                    return Result::Committed;
                },
                0, 1000, hooks);
        });

        queue.take()();
        worker.join();

        QVERIFY(observedLostCancellation);
        QVERIFY(actual == Result::Committed);
        QCOMPARE(*counter, 1);
    }

    void completedAtTimeoutBoundary() {
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        bool observedLostCancellation = false;
        WaitHooks hooks;
        hooks.firstWait = [](QSemaphore &done, int) {
            // Force the timeout branch with state already Done and
            // its terminal notification still available.
            acquireOrAbort(done);
            done.release();
            return false;
        };
        hooks.cancelLost = [&] {
            observedLostCancellation = true;
        };

        Result actual = Result::NotCommitted;
        std::thread worker([&] {
            actual = dispatcher.call(
                [] { return Result::Committed; },
                0, 1000, hooks);
        });

        queue.take()();
        worker.join();

        QVERIFY(observedLostCancellation);
        QVERIFY(actual == Result::Committed);
    }

    void shutdownCancelsPendingAndRejectsNewWork() {
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        const auto counter = std::make_shared<int>(0);
        Result pendingResult = Result::Committed;
        std::thread worker([&] {
            pendingResult = dispatcher.call(
                [counter] {
                    ++*counter;
                    return Result::Committed;
                },
                5000, 1000);
        });

        Task queued = queue.take();
        dispatcher.stop();
        worker.join();

        QVERIFY(pendingResult == Result::NotCommitted);
        queued();
        QCOMPARE(*counter, 0);

        Result newResult = Result::Committed;
        std::thread rejected([&] {
            newResult = dispatcher.call(
                [counter] {
                    ++*counter;
                    return Result::Committed;
                },
                5000, 1000);
        });
        rejected.join();

        QVERIFY(newResult == Result::NotCommitted);
        QCOMPARE(queue.queued(), 0);
        QCOMPARE(*counter, 0);

        dispatcher.stop(); // Idempotent.
    }

    void enqueueFailure_data() {
        QTest::addColumn<bool>("throwOnPost");
        QTest::newRow("false") << false;
        QTest::newRow("exception") << true;
    }

    void enqueueFailure() {
        QFETCH(bool, throwOnPost);
        Dispatcher dispatcher(
            QThread::currentThread(),
            [throwOnPost](Task) -> bool {
                if (throwOnPost) {
                    throw std::runtime_error("enqueue failure");
                }
                return false;
            });

        bool executed = false;
        Result actual = Result::Committed;
        std::thread worker([&] {
            actual = dispatcher.call(
                [&] {
                    executed = true;
                    return Result::Committed;
                },
                5000, 1000);
        });
        worker.join();

        QVERIFY(actual == Result::NotCommitted);
        QVERIFY(!executed);
    }

    void invalidArguments() {
        ManualQueue queue;
        Dispatcher dispatcher(
            QThread::currentThread(),
            [&](Task task) { return queue.post(std::move(task)); });

        Result empty = Result::Committed;
        Result negativeTimeout = Result::Committed;
        Result invalidBudget = Result::Committed;

        std::thread worker([&] {
            empty = dispatcher.call({}, 0, 1000);
            negativeTimeout = dispatcher.call(
                [] { return Result::Committed; }, -1, 1000);
            invalidBudget = dispatcher.call(
                [] { return Result::Committed; }, 0, 0);
        });
        worker.join();

        QVERIFY(empty == Result::Failed);
        QVERIFY(negativeTimeout == Result::Failed);
        QVERIFY(invalidBudget == Result::Failed);
        QCOMPARE(queue.queued(), 0);
    }

    void uiThreadMisuse_data() {
        QTest::addColumn<QString>("argument");
        QTest::newRow("direct")
            << QStringLiteral("--probe-ui");
        QTest::newRow("reentrant")
            << QStringLiteral("--probe-reentrant");
    }

    void uiThreadMisuse() {
        QFETCH(QString, argument);

        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments(QStringList{argument});
        child.start();

        QVERIFY(child.waitForStarted(5000));
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        QVERIFY(child.readAllStandardError().contains(
            "must not be called on the UI thread"));
    }

    void realQtDispatchAndShutdown() {
        QObject context;
        initializeUiThreadDispatcher(&context);

        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(
            &watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        watchdog.start(5000);

        bool onUiThread = false;
        Result actual = Result::Failed;
        std::thread worker([&] {
            actual = runOnUiThreadAndWait(
                [&] {
                    onUiThread =
                        QThread::currentThread() == context.thread();
                    loop.quit();
                    return Result::Committed;
                },
                5000, 1000);
        });

        loop.exec();
        worker.join();
        watchdog.stop();

        QVERIFY(onUiThread);
        QVERIFY(actual == Result::Committed);

        shutdownUiThreadDispatcher();

        bool executed = false;
        Result rejected = Result::Committed;
        std::thread afterShutdown([&] {
            rejected = runOnUiThreadAndWait(
                [&] {
                    executed = true;
                    return Result::Committed;
                },
                5000, 1000);
        });
        afterShutdown.join();

        QVERIFY(rejected == Result::NotCommitted);
        QVERIFY(!executed);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();

    if (arguments.contains(QStringLiteral("--probe-ui")) ||
        arguments.contains(QStringLiteral("--probe-reentrant"))) {
        QObject context;
        initializeUiThreadDispatcher(&context);

        if (arguments.contains(QStringLiteral("--probe-ui"))) {
            const auto result = runOnUiThreadAndWait(
                [] { return Result::Committed; }, 0, 1000);
            Q_UNUSED(result);
            return 0; // Parent must fail if assertions did not fire.
        }

        std::thread worker([] {
            const auto result = runOnUiThreadAndWait(
                [] {
                    return runOnUiThreadAndWait(
                        [] { return Result::Committed; }, 0, 1000);
                },
                5000, 1000);
            Q_UNUSED(result);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), "quit",
                Qt::QueuedConnection);
        });

        QTimer::singleShot(5000, &app, [] {
            QCoreApplication::quit();
        });
        app.exec();
        worker.join();
        return 0;
    }

    UiThreadTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_ui_thread.moc"
