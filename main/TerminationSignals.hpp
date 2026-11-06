#pragma once

#include <QObject>
#include <QTimer>
#include <csignal>
#include <functional>
#include <utility>

#ifdef Q_OS_UNIX
#include <QSocketNotifier>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

// Only signal-safe operations occur in the handler. Qt work runs on the
// application's thread, including when multiple termination signals arrive.
class TerminationSignals : public QObject {
public:
    TerminationSignals(QObject *parent, std::function<void()> quit)
        : QObject(parent), quit_(std::move(quit)) {
#ifdef Q_OS_UNIX
        if (::pipe(pipe_) != 0) qFatal("Cannot create termination signal pipe");
        for (int fd : pipe_) {
            if (::fcntl(fd, F_SETFL, O_NONBLOCK) == -1 ||
                ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
                qFatal("Cannot configure termination signal pipe");
        }
        writeFd_ = pipe_[1];
        notifier_ = new QSocketNotifier(pipe_[0], QSocketNotifier::Read, this);
        connect(notifier_, &QSocketNotifier::activated, this, [this] {
            char bytes[64];
            while (::read(pipe_[0], bytes, sizeof(bytes)) > 0) {}
            quit_();
        });
        struct sigaction action {};
        action.sa_handler = handle;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (::sigaction(SIGTERM, &action, &oldTerm_) != 0 ||
            ::sigaction(SIGINT, &action, &oldInt_) != 0)
            qFatal("Cannot install termination signal handlers");
#else
        oldTerm_ = std::signal(SIGTERM, handle);
        oldInt_ = std::signal(SIGINT, handle);
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            if (requested_) {
                requested_ = 0;
                quit_();
            }
        });
        timer->start(50);
#endif
    }

    ~TerminationSignals() override {
#ifdef Q_OS_UNIX
        ::sigaction(SIGTERM, &oldTerm_, nullptr);
        ::sigaction(SIGINT, &oldInt_, nullptr);
        writeFd_ = -1;
        notifier_->setEnabled(false);
        ::close(pipe_[0]);
        ::close(pipe_[1]);
#else
        std::signal(SIGTERM, oldTerm_);
        std::signal(SIGINT, oldInt_);
#endif
    }

private:
    static void handle(int) {
#ifdef Q_OS_UNIX
        const int savedErrno = errno;
        const char byte = 1;
        const int fd = writeFd_;
        if (fd >= 0) {
            // A full pipe already represents a pending quit request.
            while (::write(fd, &byte, sizeof(byte)) == -1 && errno == EINTR) {}
        }
        errno = savedErrno;
#else
        requested_ = 1;
#endif
    }

    std::function<void()> quit_;
#ifdef Q_OS_UNIX
    inline static volatile std::sig_atomic_t writeFd_ = -1;
    int pipe_[2] = {-1, -1};
    QSocketNotifier *notifier_ = nullptr;
    struct sigaction oldTerm_ {}, oldInt_ {};
#else
    inline static volatile std::sig_atomic_t requested_ = 0;
    using Handler = void (*)(int);
    Handler oldTerm_, oldInt_;
#endif
};
