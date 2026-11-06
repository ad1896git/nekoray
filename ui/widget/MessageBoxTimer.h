#pragma once

#include <QMessageBox>
#include <QTimer>
#include <QSet>
#include "main/ShutdownDialogs.hpp"

class MessageBoxTimer : public QTimer {
public:
    QMessageBox *msgbox = nullptr;
    bool showed = false;

    explicit MessageBoxTimer(QObject *parent, QMessageBox *msgbox, int delayMs) : QTimer(parent) {
        connect(this, &QTimer::timeout, this, &MessageBoxTimer::timeoutFunc, Qt::ConnectionType::QueuedConnection);
        timers_.insert(this);
        this->msgbox = msgbox;
        setSingleShot(true);
        setInterval(delayMs);
        if (ShutdownDialogs::suppressed) cancel();
        else start();
    };

    ~MessageBoxTimer() override { timers_.remove(this); }

    static void cancelAll() {
        // Copy because rejecting a visible dialog can resume its caller.
        const auto timers = timers_;
        for (auto *timer : timers) timer->cancel();
    }

    void cancel() {
        cancelled_ = true;
        QTimer::stop();
        if (msgbox != nullptr && showed) {
            msgbox->reject(); // return the timeoutFunc
        }
    };

private:
    inline static QSet<MessageBoxTimer *> timers_;
    bool cancelled_ = false;

    void timeoutFunc() {
        // stop() cannot recall an already queued timeout connection.
        if (cancelled_ || ShutdownDialogs::suppressed || msgbox == nullptr) return;
        showed = true;
        msgbox->exec();
        msgbox = nullptr;
    }
};
