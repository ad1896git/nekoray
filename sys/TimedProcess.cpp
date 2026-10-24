#include "TimedProcess.hpp"

namespace NekoGui_sys {

TimedProcess::TimedProcess(QObject *parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        complete(TimedOut, -1);
    });
}

TimedProcess::~TimedProcess() {
    done_ = true;
    deadline_.stop();

    if (process_) {
        QObject::disconnect(process_.data(), nullptr, this, nullptr);
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
        } else {
            process_->deleteLater();
        }
    }
}

void TimedProcess::start(const QString &program,
                         const QStringList &arguments,
                         int timeoutMs) {
    if (started_ || done_) return;
    started_ = true;

    auto *process = new QProcess;
    process_ = process;
    process->setProgram(program);
    process->setArguments(arguments);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());

    // These connections outlive the request owner and reap the process.
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            process, [process](int, QProcess::ExitStatus) {
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred,
            process, [process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) process->deleteLater();
            });

    // Cancellation can happen while launch is still in progress.
    const QPointer<TimedProcess> owner(this);
    connect(process, &QProcess::started, process, [owner, process] {
        if (!owner || owner->done_) process->kill();
    });

    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                complete(status == QProcess::NormalExit && exitCode == 0
                             ? Succeeded : Failed,
                         status == QProcess::NormalExit ? exitCode : -1);
            });
    connect(process, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) complete(FailedToStart, -1);
            });

    deadline_.start(qMax(1, timeoutMs));
    process->start();
}

void TimedProcess::cancel() {
    complete(Cancelled, -1);
}

void TimedProcess::complete(Outcome outcome, int exitCode) {
    if (done_) return;
    done_ = true;
    deadline_.stop();

    if ((outcome == TimedOut || outcome == Cancelled) && process_ &&
        process_->state() != QProcess::NotRunning) {
        process_->kill();
    }

    emit finished(static_cast<int>(outcome), exitCode);
}

} // namespace NekoGui_sys
