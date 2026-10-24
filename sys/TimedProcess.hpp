#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

namespace NekoGui_sys {

class TimedProcess final : public QObject {
    Q_OBJECT

public:
    enum Outcome { Succeeded, FailedToStart, Failed, TimedOut, Cancelled };
    Q_ENUM(Outcome)

    explicit TimedProcess(QObject *parent = nullptr);
    ~TimedProcess() override;

    // One execution per instance. All methods run on the owning thread.
    void start(const QString &program, const QStringList &arguments,
               int timeoutMs = 120000);
    void cancel();

signals:
    void finished(int outcome, int exitCode);

private:
    void complete(Outcome outcome, int exitCode);

    QPointer<QProcess> process_;
    QTimer deadline_;
    bool started_ = false;
    bool done_ = false;
};

} // namespace NekoGui_sys
