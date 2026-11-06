#include "main/CoordinatedApplication.hpp"
#include "main/TerminationSignals.hpp"
#include "main/NekoGui_UiThread.hpp"
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <thread>

// A separate process per entry route exercises real event-loop teardown.
int main(int argc, char **argv) {
    CoordinatedApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QObject context;
    bool cleaned = false, dispatcherStopped = false, failed = false;
    int cleanups = 0;
    app.setShutdownHandler([&] {
        if (cleaned) return true;
        if (cleanups) return false;
        ++cleanups;
        if (mode == "burst") {
            // Repeat after cleanup begins, while the nested loop pumps events.
            QTimer::singleShot(10, &context, [] {
                for (int i = 0; i < 100; ++i) std::raise(i % 2 ? SIGTERM : SIGINT);
            });
        }
        if (dispatcherStopped || QThread::currentThread() != app.thread()) failed = true;
        // Cleanup must be able to deliver queued UI work before shutdown.
        bool delivered = false;
        QEventLoop loop;
        UiThreadResult result = UiThreadResult::NotCommitted;
        std::thread worker([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            result = runOnUiThreadAndWait([&] {
                delivered = true;
                return UiThreadResult::Committed;
            }, 1000, 200);
            QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
        });
        loop.exec();
        worker.join();
        if (!delivered || result != UiThreadResult::Committed) failed = true;
        cleaned = true;
        return true;
    });
    initializeUiThreadDispatcher(&context);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &context, [&] {
        dispatcherStopped = true;
        if (!cleaned) failed = true;
    });
    TerminationSignals terminationSignals(&app, [&] { app.quit(); });
    QTimer::singleShot(0, &app, [&] {
        if (mode == "term") std::raise(SIGTERM);
        else if (mode == "int") std::raise(SIGINT);
        else if (mode == "burst") {
            for (int i = 0; i < 100; ++i) std::raise(i % 2 ? SIGTERM : SIGINT);
        } else if (mode == "exit") app.exit();
        else app.quit();
    });
    QTimer::singleShot(5000, &app, [&] { failed = true; app.exit(1); });
    const int rc = app.exec();
    if (rc || failed || !dispatcherStopped || cleanups != 1) {
        std::fprintf(stderr, "coordinated quit failed: rc=%d cleanups=%d failed=%d stopped=%d\n",
                     rc, cleanups, failed, dispatcherStopped);
        return 1;
    }
    return 0;
}
