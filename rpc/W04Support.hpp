#pragma once

#include <QElapsedTimer>
#include <QSemaphore>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace NkrW04 {

constexpr int StopMode = 114514;
constexpr int ExternalWaitMs = 2000;
constexpr int ExternalWarmupMs = 500;
constexpr int RpcMarginMs = 5000;

// full_speed_timeout is seconds, unlike TestReq::timeout.
inline qint64 testTimeoutMs(int requestMs, bool fullTest,
                            bool fullSpeed, int downloadSeconds) {
    qint64 result = qMax<qint64>(1, requestMs) + RpcMarginMs;
    if (fullTest && fullSpeed) {
        const qint64 seconds = downloadSeconds > 0 ? downloadSeconds : 30;
        result += seconds * 1000;
    }
    return result;
}

class Finally {
public:
    explicit Finally(std::function<void()> fn) : fn_(std::move(fn)) {}
    Finally(const Finally &) = delete;
    Finally &operator=(const Finally &) = delete;
    ~Finally() { fn_(); }

private:
    std::function<void()> fn_;
};

// One waiting caller. Completion can precede the wait.
// A timed-out caller never exposes its stack to a late completion.
template<class T>
class Completion {
public:
    bool complete(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_ || abandoned_) return false;
            value_ = std::move(value);
            done_ = true;
        }
        semaphore_.release();
        return true;
    }

    bool abandoned() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return abandoned_;
    }

    // timeoutMs == 0 preserves the existing unlimited-call contract.
    bool wait(qint64 timeoutMs, T &out) {
        QElapsedTimer elapsed;
        elapsed.start();

        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (done_) {
                    out = value_;
                    return true;
                }
                if (abandoned_) return false;
            }

            qint64 remaining = 1000;
            if (timeoutMs > 0) {
                remaining = timeoutMs - elapsed.elapsed();
                if (remaining <= 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (done_) {
                        out = value_;
                        return true;
                    }
                    abandoned_ = true;
                    return false;
                }
            }

            semaphore_.tryAcquire(
                1, static_cast<int>(qMin<qint64>(remaining, 1000)));
        }
    }

private:
    mutable std::mutex mutex_;
    QSemaphore semaphore_;
    T value_{};
    bool done_ = false;
    bool abandoned_ = false;
};

struct Batch {
    std::atomic_bool speedtest_cancel{false};
    // Guarded by BatchGate::mutex_.
    int externalPending = 0;
    bool workersFinished = false;
};

enum class Phase {
    Idle,
    Running,
    Cancelling
};

class BatchGate {
public:
    // UI thread calls begin(); only the controller calls finish().
    std::shared_ptr<Batch> begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) return {};
        active_ = std::make_shared<Batch>();
        latest_ = active_;
        return active_;
    }

    // Call before selection, archive, or already-running validation.
    bool dispatchStop(int mode) {
        if (mode != StopMode) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) active_->speedtest_cancel.store(true);
        return true;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return Phase::Idle;
        return (active_->workersFinished || active_->speedtest_cancel.load())
                   ? Phase::Cancelling : Phase::Running;
    }

    bool isLatest(const std::shared_ptr<Batch> &batch) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_ == batch;
    }

    void retainExternal(const std::shared_ptr<Batch> &batch) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++batch->externalPending;
    }

    // Only confirmed owner-thread disposal releases external admission.
    bool releaseExternal(const std::shared_ptr<Batch> &batch) {
        std::lock_guard<std::mutex> lock(mutex_);
        Q_ASSERT(batch->externalPending > 0);
        --batch->externalPending;
        return finishIfReady(batch);
    }

    bool finish(const std::shared_ptr<Batch> &batch) {
        std::lock_guard<std::mutex> lock(mutex_);
        batch->workersFinished = true;
        return finishIfReady(batch);
    }

private:
    bool finishIfReady(const std::shared_ptr<Batch> &batch) {
        if (active_ != batch || !batch->workersFinished || batch->externalPending)
            return false;
        active_.reset();
        return true;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<Batch> active_;
    std::shared_ptr<Batch> latest_;
};

// Returns only after every successfully created worker has been joined.
// Exceptions and thread-creation failures cancel the batch and still join.
template<class Work>
bool runBatch(int configuredConcurrency, int itemCount,
              const std::shared_ptr<Batch> &batch, Work work) {
    const int count = qMin(qMax(1, configuredConcurrency), qMax(0, itemCount));
    std::atomic_int next{0};
    std::atomic_bool failed{false};
    std::vector<std::thread> workers;

    try {
        workers.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            workers.emplace_back([&] {
                try {
                    for (;;) {
                        if (batch->speedtest_cancel.load()) return;
                        const int index = next.fetch_add(1);
                        if (index >= itemCount) return;
                        if (batch->speedtest_cancel.load()) return;
                        work(index);
                    }
                } catch (...) {
                    failed.store(true);
                    batch->speedtest_cancel.store(true);
                }
            });
        }
    } catch (...) {
        failed.store(true);
        batch->speedtest_cancel.store(true);
    }

    for (auto &worker : workers) {
        if (worker.joinable()) worker.join();
    }
    return !failed.load();
}

// Dispatch functions must serialize callbacks on one owner thread.
// create/dispose run only there. dispose must not throw.
// delay must schedule its callback on that same owner thread.
//
// Pending start: cancellation prevents creation.
// Running start: cancellation causes compensating disposal.
// Waiting caller: timeout never destroys resources on the caller's thread.
class OwnerLease : public std::enable_shared_from_this<OwnerLease> {
public:
    using Task = std::function<void()>;
    using Dispatch = std::function<void(Task)>;

    OwnerLease(Dispatch post, Dispatch delay, Task create, Task dispose,
               Task onClosed = {})
        : post_(std::move(post)),
          delay_(std::move(delay)),
          create_(std::move(create)),
          dispose_(std::move(dispose)),
          onClosed_(std::move(onClosed)) {}

    bool start(int timeoutMs) {
        const auto self = shared_from_this();
        post_([self] {
            if (self->cancelled_.load()) {
                self->cleanup();
                self->ready_.complete(false);
                return;
            }

            self->created_ = true;
            try {
                self->create_();
                if (self->cancelled_.load()) {
                    self->cleanup();
                    self->ready_.complete(false);
                    return;
                }

                self->delay_([self] {
                    if (self->cancelled_.load()) {
                        self->cleanup();
                        self->ready_.complete(false);
                    } else {
                        self->ready_.complete(true);
                    }
                });
            } catch (...) {
                self->cancelled_.store(true);
                self->cleanup();
                self->ready_.complete(false);
            }
        });

        bool ready = false;
        if (ready_.wait(timeoutMs, ready) && ready) return true;
        requestStop();
        return false;
    }

    bool stop(int timeoutMs) {
        requestStop();
        bool closed = false;
        return closed_.wait(timeoutMs, closed) && closed;
    }

private:
    void requestStop() {
        cancelled_.store(true);
        if (stopPosted_.exchange(true)) return;
        const auto self = shared_from_this();
        post_([self] { self->cleanup(); });
    }

    // Owner-thread-only fields and method.
    void cleanup() {
        if (cleaned_) return;
        cleaned_ = true;
        if (created_) dispose_();
        if (onClosed_) onClosed_();
        closed_.complete(true);
    }

    Dispatch post_;
    Dispatch delay_;
    Task create_;
    Task dispose_;
    Task onClosed_;

    Completion<bool> ready_;
    Completion<bool> closed_;
    std::atomic_bool cancelled_{false};
    std::atomic_bool stopPosted_{false};

    bool created_ = false;
    bool cleaned_ = false;
};

} // namespace NkrW04
