#include "ui/widget/MessageBoxTimer.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QObject context;
    QMessageBox box(QMessageBox::Warning, "pending", "must never execute");
    MessageBoxTimer timer(&context, &box, 100);
    // Queue the signal before cancellation. QTimer::stop alone cannot recall
    // the queued timeoutFunc connection, the original modal regression.
    QMetaObject::invokeMethod(&timer, "timeout", Qt::DirectConnection);
    timer.cancel();
    QEventLoop loop;
    bool watchdog = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    QTimer::singleShot(800, &context, [&] { watchdog = true; box.reject(); });
    loop.exec();
    if (timer.showed || watchdog || elapsed.elapsed() > 500) {
        std::fprintf(stderr, "cancelled queued timer entered a modal loop\n");
        return 1;
    }
    MessageBoxTimer existing(&context, &box, 1);
    ShutdownDialogs::suppressed = true;
    MessageBoxTimer::cancelAll();
    MessageBoxTimer late(&context, &box, 1);
    QMetaObject::invokeMethod(&existing, "timeout", Qt::DirectConnection);
    QMetaObject::invokeMethod(&late, "timeout", Qt::DirectConnection);
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
    return existing.isActive() || late.isActive() || existing.showed || late.showed ? 1 : 0;
}
