#pragma once

#include <QApplication>
#include <QEvent>
#include <functional>
#include <utility>

// Install the aboutToQuit fallback before MainWindow creates its dispatcher.
class CoordinatedApplication : public QApplication {
public:
    CoordinatedApplication(int &argc, char **argv) : QApplication(argc, argv) {
        connect(this, &QCoreApplication::aboutToQuit, this, [this] {
            if (shutdown_ && !shutdown_())
                qWarning("Application exited before coordinated shutdown completed");
        }, Qt::DirectConnection);
    }

    void setShutdownHandler(std::function<bool()> shutdown) {
        shutdown_ = std::move(shutdown);
    }

protected:
    bool event(QEvent *event) override {
        // Native macOS Quit and QCoreApplication::quit arrive here while the
        // UI dispatcher is still available. A failed/reentrant request waits.
        if (event->type() == QEvent::Quit && shutdown_ && !shutdown_()) {
            event->ignore();
            return true;
        }
        return QApplication::event(event);
    }

private:
    std::function<bool()> shutdown_;
};
