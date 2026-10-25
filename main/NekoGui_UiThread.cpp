#include "NekoGui_UiThread.hpp"
#include "NekoGui_UiThread_p.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <memory>
#include <mutex>
#include <utility>

namespace {

std::mutex dispatcherMutex;
std::shared_ptr<NekoGui_detail::UiThreadDispatcher> dispatcher;

std::shared_ptr<NekoGui_detail::UiThreadDispatcher> currentDispatcher() {
    std::lock_guard<std::mutex> lock(dispatcherMutex);
    return dispatcher;
}

} // namespace

void initializeUiThreadDispatcher(QObject *context) {
    auto *app = QCoreApplication::instance();
    if (context == nullptr || app == nullptr ||
        QThread::currentThread() != app->thread() ||
        context->thread() != app->thread()) {
        qFatal("initializeUiThreadDispatcher requires a UI-thread context");
    }

    auto value = std::make_shared<NekoGui_detail::UiThreadDispatcher>(
        app->thread(),
        [context](NekoGui_detail::UiTask task) {
            return QMetaObject::invokeMethod(
                context, std::move(task), Qt::QueuedConnection);
        });

    {
        std::lock_guard<std::mutex> lock(dispatcherMutex);
        if (dispatcher) {
            qFatal("initializeUiThreadDispatcher must be called only once");
        }
        dispatcher = value;
    }

    QObject::connect(
        app, &QCoreApplication::aboutToQuit, context,
        [value] { value->stop(); }, Qt::DirectConnection);

    // Fallback for context destruction. MainWindow explicitly closes
    // the gate earlier, before destroying its own members.
    QObject::connect(
        context, &QObject::destroyed,
        [value] { value->stop(); });
}

void shutdownUiThreadDispatcher() {
    if (const auto value = currentDispatcher()) {
        value->stop();
    }
}

UiThreadResult runOnUiThreadAndWait(
    std::function<UiThreadResult()> callback,
    int queueTimeoutMs,
    int runningBudgetMs) {
    const auto value = currentDispatcher();
    if (!value) {
        return UiThreadResult::NotCommitted;
    }
    return value->call(
        std::move(callback), queueTimeoutMs, runningBudgetMs);
}
