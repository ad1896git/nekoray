#pragma once

#include <future>
#include <QThread>
#include <QDebug>
#include <exception>

// FOR OLD QT

class QThreadCreateThread : public QThread {
public:
    explicit QThreadCreateThread(std::future<void> &&future)
            : m_future(std::move(future)) {
        // deleteLater
        connect(this, &QThread::finished, this, &QThread::deleteLater);
    }

private:
    void run() override {
        try {
            m_future.get();
        } catch (const std::exception &e) {
            qWarning("Background task failed: %s", e.what());
        } catch (...) {
            qWarning("Background task failed: unknown exception");
        }
    }

    std::future<void> m_future;
};

inline QThread *createThreadImpl(std::future<void> &&future) {
    return new QThreadCreateThread(std::move(future));
}

template<typename Function, typename... Args>
QThread *createQThread(Function &&f, Args &&... args) {
    using DecayedFunction = typename std::decay<Function>::type;
    auto threadFunction =
            [f = static_cast<DecayedFunction>(std::forward<Function>(f))](auto &&... largs) mutable -> void {
                (void) std::invoke(std::move(f), std::forward<decltype(largs)>(largs)...);
            };

    return createThreadImpl(std::async(std::launch::deferred,
                                       std::move(threadFunction),
                                       std::forward<Args>(args)...));
}
