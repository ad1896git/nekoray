#pragma once
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <functional>
#include <memory>
#include <utility>
namespace NekoGui_concurrency {
inline void RequireUiThread() {
    auto *app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread())
        qFatal("W03: model access outside the application thread");
}
inline bool PostUi(std::function<void()> callback) {
    auto *app = QCoreApplication::instance();
    if (!app || QCoreApplication::closingDown()) return false;
    return QMetaObject::invokeMethod(app, std::move(callback), Qt::QueuedConnection);
}
template<class T, class Lookup, class Apply>
bool WithRegistered(int id, const std::weak_ptr<T> &expected, Lookup lookup, Apply apply) {
    RequireUiThread();
    auto object = expected.lock();
    if (!object || lookup(id) != object) return false;
    apply(object);
    return true;
}
} // namespace NekoGui_concurrency
