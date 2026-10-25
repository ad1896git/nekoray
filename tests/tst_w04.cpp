#include <QtTest>

#include "rpc/RpcTransport.hpp"
#include "rpc/W04Support.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <atomic>
#include <climits>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using Task = NkrW04::OwnerLease::Task;

class Queue {
public:
    void post(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    Task take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty()) return {};
        auto result = std::move(tasks_.front());
        tasks_.pop_front();
        return result;
    }

    void drain() {
        for (;;) {
            auto task = take();
            if (!task) return;
            task();
        }
    }

private:
    std::mutex mutex_;
    std::deque<Task> tasks_;
};

struct Resources {
    int created = 0;
    int disposed = 0;
    int live = 0;
};

class Join {
public:
    explicit Join(std::thread &thread) : thread_(thread) {}
    ~Join() {
        if (thread_.joinable()) thread_.join();
    }
private:
    std::thread &thread_;
};

// Processes network/server events while a synchronous call runs elsewhere.
bool pumpUntil(const std::atomic_bool &done, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done.load() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    return done.load();
}

} // namespace

class W04Test : public QObject {
    Q_OBJECT

private slots:
    void completionBeforeWait() {
        NkrW04::Completion<int> completion;
        QVERIFY(completion.complete(42));
        QVERIFY(!completion.complete(99));
        int value = 0;
        QVERIFY(completion.wait(100, value));
        QCOMPARE(value, 42);
    }

    void lateCompletionCannotPublish() {
        const auto completion =
            std::make_shared<NkrW04::Completion<int>>();
        const std::weak_ptr<NkrW04::Completion<int>> weak = completion;

        int value = 7;
        QVERIFY(!completion->wait(20, value));
        QCOMPARE(value, 7);
        QVERIFY(completion->abandoned());

        auto late = [completion] {
            return completion->complete(99);
        };
        QVERIFY(!late());
        QVERIFY(!weak.expired());
    }

    void timeoutUnitsAndOverflow() {
        QCOMPARE(NkrW04::testTimeoutMs(10000, false, false, 30),
                 qint64(15000));
        QCOMPARE(NkrW04::testTimeoutMs(10000, true, false, 30),
                 qint64(15000));
        QCOMPARE(NkrW04::testTimeoutMs(10000, true, true, 30),
                 qint64(45000));
        QCOMPARE(NkrW04::testTimeoutMs(10000, true, true, 0),
                 qint64(45000));
        QCOMPARE(NkrW04::testTimeoutMs(10000, true, true, -1),
                 qint64(45000));
        QCOMPARE(NkrW04::testTimeoutMs(10000, true, true, INT_MAX),
                 qint64(INT_MAX) * 1000 + 15000);
    }

    void nonpositiveConcurrency_data() {
        QTest::addColumn<int>("concurrency");
        QTest::newRow("zero") << 0;
        QTest::newRow("negative") << -1;
        QTest::newRow("minimum-int") << INT_MIN;
        QTest::newRow("normal") << 4;
    }

    void nonpositiveConcurrency() {
        QFETCH(int, concurrency);
        auto batch = std::make_shared<NkrW04::Batch>();
        std::atomic_int visits{0};
        QVERIFY(NkrW04::runBatch(
            concurrency, 17, batch,
            [&](int) { visits.fetch_add(1); }));
        QCOMPARE(visits.load(), 17);
    }

    void failedTaskDoesNotLoseWorkerCompletion() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        QVERIFY(batch);
        std::atomic_int visits{0};

        // Models an RPC failure's early return from a task.
        QVERIFY(NkrW04::runBatch(2, 20, batch, [&](int) {
            visits.fetch_add(1);
            return;
        }));
        QCOMPARE(visits.load(), 20);
        gate.finish(batch);
        QVERIFY(gate.phase() == NkrW04::Phase::Idle);
        QVERIFY(gate.begin());
    }

    void exceptionJoinsAndAllowsAnotherRound() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        QVERIFY(!NkrW04::runBatch(3, 20, batch, [](int) {
            throw std::runtime_error("injected");
        }));
        QVERIFY(batch->speedtest_cancel.load());
        gate.finish(batch);
        QVERIFY(gate.begin());
    }

    void stopIsDispatchedBeforeStartValidation() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        QVERIFY(batch);

        int selectionReads = 0;
        const auto entry = [&](int mode) {
            if (gate.dispatchStop(mode)) return;
            ++selectionReads;
        };

        entry(NkrW04::StopMode);
        QCOMPARE(selectionReads, 0);
        QVERIFY(gate.phase() == NkrW04::Phase::Cancelling);
        QVERIFY(!gate.begin());
        gate.finish(batch);
        QVERIFY(gate.phase() == NkrW04::Phase::Idle);
    }

    void externalCleanupKeepsAdmissionClosed() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        gate.retainExternal(batch);
        gate.retainExternal(batch);
        QVERIFY(gate.dispatchStop(NkrW04::StopMode));
        QVERIFY(!gate.finish(batch));
        QVERIFY(gate.phase() == NkrW04::Phase::Cancelling);
        QVERIFY(!gate.begin());
        QVERIFY(!gate.releaseExternal(batch));
        QVERIFY(gate.phase() == NkrW04::Phase::Cancelling);
        QVERIFY(!gate.begin());
        QVERIFY(gate.releaseExternal(batch));
        QVERIFY(gate.phase() == NkrW04::Phase::Idle);
        QVERIFY(gate.begin());
    }

    void lateOwnerCleanupReopensAdmission() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        Queue owner;
        const auto resources = std::make_shared<Resources>();
        auto lease = std::make_shared<NkrW04::OwnerLease>(
            [&](Task task) { owner.post(std::move(task)); },
            [](Task task) { task(); },
            [resources] { ++resources->created; ++resources->live; },
            [resources] { ++resources->disposed; --resources->live; },
            [&gate, batch] { gate.releaseExternal(batch); });
        gate.retainExternal(batch);
        QVERIFY(!lease->start(20));
        QVERIFY(!lease->stop(20));
        QVERIFY(!gate.finish(batch));
        QVERIFY(gate.phase() == NkrW04::Phase::Cancelling);
        QVERIFY(!gate.begin());
        lease.reset();
        owner.drain();
        QCOMPARE(resources->created, 0);
        QVERIFY(gate.phase() == NkrW04::Phase::Idle);
        QVERIFY(gate.begin());
    }

    void twentyStopRestartRounds() {
        NkrW04::BatchGate gate;
        for (int round = 0; round < 20; ++round) {
            const auto batch = gate.begin();
            QVERIFY(batch);

            QSemaphore entered;
            QSemaphore release;
            std::atomic_int visits{0};

            std::thread controller([&] {
                NkrW04::runBatch(1, 20, batch, [&](int) {
                    visits.fetch_add(1);
                    entered.release();
                    release.acquire();
                });
                gate.finish(batch);
            });
            Join join(controller);

            const bool started = entered.tryAcquire(1, 1000);
            const bool stopDispatched = gate.dispatchStop(NkrW04::StopMode);
            const bool secondRoundRejected = !gate.begin();
            release.release();
            controller.join();

            QVERIFY(started);
            QVERIFY(stopDispatched);
            QVERIFY(secondRoundRejected);
            QCOMPARE(visits.load(), 1);
            QVERIFY(gate.phase() == NkrW04::Phase::Idle);
        }
    }

    void cancelledPendingOwnerCallbackDoesNotCreate() {
        Queue owner;
        const auto resources = std::make_shared<Resources>();

        auto lease = std::make_shared<NkrW04::OwnerLease>(
            [&](Task task) { owner.post(std::move(task)); },
            [](Task task) { task(); },
            [resources] {
                ++resources->created;
                ++resources->live;
            },
            [resources] {
                ++resources->disposed;
                --resources->live;
            });

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!lease->start(30));
        QVERIFY(!lease->stop(30));
        QVERIFY(elapsed.elapsed() < 1000);

        // Worker ownership is gone before the late callback runs.
        lease.reset();
        owner.drain();

        QCOMPARE(resources->created, 0);
        QCOMPARE(resources->disposed, 0);
        QCOMPARE(resources->live, 0);
    }

    void runningOwnerCallbackCompensatesAfterTimeout() {
        Queue owner;
        const auto resources = std::make_shared<Resources>();
        QSemaphore entered;
        QSemaphore release;

        auto lease = std::make_shared<NkrW04::OwnerLease>(
            [&](Task task) { owner.post(std::move(task)); },
            [](Task task) { task(); },
            [resources, &entered, &release] {
                ++resources->created;
                ++resources->live;
                entered.release();
                release.acquire();
            },
            [resources] {
                ++resources->disposed;
                --resources->live;
            });

        std::atomic_bool startReturned{false};
        bool startOK = true;
        std::thread waiter([&] {
            startOK = lease->start(100);
            startReturned.store(true);
        });
        Join waiterJoin(waiter);

        Task start;
        QElapsedTimer polling;
        polling.start();
        while (!start && polling.elapsed() < 1000) {
            start = owner.take();
            QTest::qWait(1);
        }

        std::thread ownerThread([start] {
            if (start) start();
        });
        Join ownerJoin(ownerThread);

        const bool running = entered.tryAcquire(1, 1000);
        const bool returned = pumpUntil(startReturned, 1500);
        const bool stopOK = lease->stop(30);

        release.release();
        ownerThread.join();
        waiter.join();

        lease.reset();
        owner.drain();

        QVERIFY(running);
        QVERIFY(returned);
        QVERIFY(!startOK);
        QVERIFY(!stopOK);
        QCOMPARE(resources->created, 1);
        QCOMPARE(resources->disposed, 1);
        QCOMPARE(resources->live, 0);
    }

    void cleanupWaitIsBoundedAndOwnershipSurvives() {
        NkrW04::BatchGate gate;
        const auto batch = gate.begin();
        gate.retainExternal(batch);
        Queue cleanup;
        const auto resources = std::make_shared<Resources>();
        bool hold = false;

        auto lease = std::make_shared<NkrW04::OwnerLease>(
            [&](Task task) {
                if (hold) cleanup.post(std::move(task));
                else task();
            },
            [](Task task) { task(); },
            [resources] {
                ++resources->created;
                ++resources->live;
            },
            [resources] {
                ++resources->disposed;
                --resources->live;
            },
            [&gate, batch] { gate.releaseExternal(batch); });

        QVERIFY(lease->start(100));
        hold = true;

        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!lease->stop(30));
        QVERIFY(elapsed.elapsed() < 1000);
        QCOMPARE(resources->live, 1);
        QVERIFY(!gate.finish(batch));
        QVERIFY(gate.phase() == NkrW04::Phase::Cancelling);
        QVERIFY(!gate.begin());

        lease.reset();
        cleanup.drain();
        QCOMPARE(resources->disposed, 1);
        QCOMPARE(resources->live, 0);
        QVERIFY(gate.phase() == NkrW04::Phase::Idle);
        QVERIFY(gate.begin());
    }

    void transportSuccess() {
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost),
                 qPrintable(server.errorString()));

        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                auto input = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [socket, input] {
                    *input += socket->readAll();
                    if (socket->property("answered").toBool()) return;
                    if (!input->contains("\r\n\r\n") ||
                        !input->endsWith("x")) return;
                    socket->setProperty("answered", true);
                    socket->write(
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 2\r\n"
                        "grpc-status: 0\r\n"
                        "Connection: close\r\n\r\nok");
                    socket->disconnectFromHost();
                });
            }
        });

        NkrW04::RpcTransport transport;
        QNetworkRequest request(
            QUrl(QStringLiteral("http://127.0.0.1:%1/Test")
                     .arg(server.serverPort())));

        NkrW04::WireReply result;
        std::atomic_bool done{false};
        std::thread caller([&] {
            result = transport.send(request, "x", 1000);
            done.store(true);
        });
        Join join(caller);
        const bool finished = pumpUntil(done, 2000);
        caller.join();

        QVERIFY(finished);
        QCOMPARE(result.error, QNetworkReply::NoError);
        QCOMPARE(result.body, QByteArray("ok"));
        QCOMPARE(result.grpcStatus, QByteArray("0"));
    }

    void stalledPeerAndTransportDestructionAreBounded() {
        QTcpServer server;
        QVERIFY2(server.listen(QHostAddress::LocalHost),
                 qPrintable(server.errorString()));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [socket] { socket->readAll(); });
                // Intentionally never send a response.
            }
        });

        const QUrl url(
            QStringLiteral("http://127.0.0.1:%1/Test")
                .arg(server.serverPort()));

        NkrW04::WireReply result;
        std::atomic_bool done{false};
        QElapsedTimer elapsed;
        elapsed.start();

        std::thread caller([&] {
            {
                NkrW04::RpcTransport transport;
                result = transport.send(QNetworkRequest(url), "x", 100);
            } // Channel destruction is included in this timing.
            done.store(true);
        });
        Join join(caller);
        const bool finished = pumpUntil(done, 1500);
        caller.join();

        QVERIFY(finished);
        QVERIFY(elapsed.elapsed() < 1500);
        QCOMPARE(result.error, QNetworkReply::TimeoutError);
    }
};

QTEST_GUILESS_MAIN(W04Test)
#include "tst_w04.moc"
