#pragma once

#include "NekoGui_UiThread.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace NekoGui_detail {

using UiTask = std::function<void()>;
using UiPost = std::function<bool(UiTask)>;

// Internal deterministic-test seams. Production supplies empty hooks.
struct UiWaitHooks {
    std::function<bool(QSemaphore &, int)> firstWait;
    std::function<void()> cancelLost;
};

struct UiTxn {
    enum State {
        Pending,
        Running,
        Cancelled,
        Done
    };

    std::atomic<int> state{Pending};
    UiThreadResult result = UiThreadResult::Failed;
    QSemaphore done{0};

    bool start() {
        int expected = Pending;
        return state.compare_exchange_strong(expected, Running);
    }

    bool cancel() {
        int expected = Pending;
        if (!state.compare_exchange_strong(expected, Cancelled)) {
            return false;
        }
        done.release();
        return true;
    }

    UiThreadResult outcome() const {
        if (state.load() == Cancelled) {
            return UiThreadResult::NotCommitted;
        }
        return result;
    }

    UiThreadResult wait(int timeoutMs, const UiWaitHooks &hooks) {
        const bool completed = hooks.firstWait
            ? hooks.firstWait(done, timeoutMs)
            : done.tryAcquire(1, timeoutMs);

        if (completed) {
            return outcome();
        }

        if (cancel()) {
            return UiThreadResult::NotCommitted;
        }

        // Running, Done, or cancellation by shutdown won the race.
        // In every case, consume the terminal notification before reading
        // the result. In particular, never abandon Running.
        qWarning("UI commit queue deadline expired; waiting for final result");
        if (hooks.cancelLost) {
            hooks.cancelLost();
        }
        done.acquire();
        return outcome();
    }

    void execute(const std::function<UiThreadResult()> &callback,
                 int runningBudgetMs) {
        struct Completion {
            UiTxn &txn;
            int budgetMs;
            QElapsedTimer timer;

            Completion(UiTxn &value, int budget)
                : txn(value), budgetMs(budget) {
                timer.start();
            }

            ~Completion() noexcept {
                const qint64 elapsed = timer.elapsed();
                qInfo() << "UI commit completed"
                        << "elapsed_ms=" << elapsed
                        << "budget_ms=" << budgetMs
                        << "result=" << static_cast<int>(txn.result);

                if (elapsed > budgetMs) {
                    qWarning() << "UI commit exceeded running budget"
                               << elapsed << budgetMs;
                }

                txn.state.store(Done);
                txn.done.release();
            }
        } completion(*this, runningBudgetMs);

        try {
            result = callback();
        } catch (...) {
            result = UiThreadResult::Failed;
            qWarning("UI commit callback threw");
        }
    }
};

class UiThreadDispatcher {
    struct Shared {
        QThread *uiThread;
        UiPost post;
        std::mutex mutex;
        bool stopping = false;
        std::vector<std::weak_ptr<UiTxn>> pending;

        Shared(QThread *thread, UiPost enqueue)
            : uiThread(thread), post(std::move(enqueue)) {}
    };

    std::shared_ptr<Shared> shared_;

public:
    // post must enqueue without executing inline, must not block waiting
    // for the UI thread, and must dispatch to uiThread.
    UiThreadDispatcher(QThread *uiThread, UiPost post)
        : shared_(std::make_shared<Shared>(uiThread, std::move(post))) {}

    UiThreadDispatcher(const UiThreadDispatcher &) = delete;
    UiThreadDispatcher &operator=(const UiThreadDispatcher &) = delete;

    void stop() {
        const auto shared = shared_;
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->stopping = true;
        for (const auto &weak : shared->pending) {
            if (const auto txn = weak.lock()) {
                txn->cancel();
            }
        }
        shared->pending.clear();
    }

    UiThreadResult call(std::function<UiThreadResult()> callback,
                        int queueTimeoutMs,
                        int runningBudgetMs,
                        const UiWaitHooks &hooks = {}) {
        const auto shared = shared_;
        const bool onUiThread =
            QThread::currentThread() == shared->uiThread;

        Q_ASSERT_X(!onUiThread, "runOnUiThreadAndWait",
                   "must not be called on the UI thread");
        if (onUiThread) {
            return UiThreadResult::Failed;
        }

        if (!callback || queueTimeoutMs < 0 || runningBudgetMs <= 0) {
            return UiThreadResult::Failed;
        }

        const auto txn = std::make_shared<UiTxn>();
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->stopping) {
                return UiThreadResult::NotCommitted;
            }

            auto &pending = shared->pending;
            pending.erase(
                std::remove_if(
                    pending.begin(), pending.end(),
                    [](const std::weak_ptr<UiTxn> &weak) {
                        return weak.expired();
                    }),
                pending.end());
            pending.emplace_back(txn);

            bool posted = false;
            try {
                posted = shared->post(
                    [shared, txn, callback = std::move(callback),
                     runningBudgetMs] {
                        {
                            std::lock_guard<std::mutex> lock(shared->mutex);
                            if (shared->stopping) {
                                txn->cancel();
                                return;
                            }
                            if (!txn->start()) {
                                return;
                            }
                        }

                        // Do not hold the dispatcher mutex while executing
                        // application code.
                        txn->execute(callback, runningBudgetMs);
                    });
            } catch (...) {
                qWarning("UI commit enqueue threw");
            }

            if (!posted) {
                // The enqueue operation runs under the same mutex used
                // before Pending -> Running, so cancellation wins here.
                txn->cancel();
            }
        }

        return txn->wait(queueTimeoutMs, hooks);
    }
};

} // namespace NekoGui_detail
