#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace NekoGui_runtime {

// Reads the configured name of a single Tun inbound from JSON/JSONC.
// An empty result means the configuration cannot supply an unambiguous name.
QString TunInterfaceName(const QByteArray &config);

class VpnProcess final : public QObject {
    Q_OBJECT

public:
    using InterfaceProbe = std::function<bool()>;

    explicit VpnProcess(InterfaceProbe interfacePresent,
                        QObject *parent = nullptr);

    // True means only that the intermediary process started.
    // Each instance represents one launch attempt.
    bool start(const QString &program, const QStringList &arguments);

    // Written to the launcher's standard input after it starts; the write
    // channel is then closed. Call before start().
    void setStandardInput(const QByteArray &input) { input_ = input; }
    // Keeps the last kMaxCapturedError bytes of standard error (each chunk is
    // still echoed to qDebug). Call before start().
    void setCaptureStandardError(bool capture) { captureError_ = capture; }
    // Requests a stop and kills the launcher, e.g. while authorization is
    // still pending. ended() follows with an empty error.
    void abort();

    QString errorString() const { return error_; }
    qint64 processId() const { return process_.processId(); }
    bool isReady() const { return ready_; }
    bool stopRequested() const { return stopRequested_; }
    void setStopRequested(bool requested) { stopRequested_ = requested; }
    int exitCode() const { return exitCode_; }
    QProcess::ExitStatus exitStatus() const { return exitStatus_; }
    QByteArray capturedStandardError() const { return capturedError_; }

    static constexpr int kMaxCapturedError = 64 * 1024;

signals:
    void ready();
    void ended(const QString &error);

private:
    void checkInterface();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);
    void readError();

    InterfaceProbe interfacePresent_;
    QProcess process_;
    QTimer poll_;
    QString error_;
    QByteArray input_;
    QByteArray capturedError_;
    int exitCode_ = -1;
    QProcess::ExitStatus exitStatus_ = QProcess::NormalExit;
    bool attempted_ = false;
    bool terminal_ = false;
    bool ready_ = false;
    bool stopRequested_ = false;
    bool captureError_ = false;
};

} // namespace NekoGui_runtime
