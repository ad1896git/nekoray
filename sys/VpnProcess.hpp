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

    QString errorString() const { return error_; }
    qint64 processId() const { return process_.processId(); }
    bool isReady() const { return ready_; }
    bool stopRequested() const { return stopRequested_; }
    void setStopRequested(bool requested) { stopRequested_ = requested; }

signals:
    void ready();
    void ended(const QString &error);

private:
    void checkInterface();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);

    InterfaceProbe interfacePresent_;
    QProcess process_;
    QTimer poll_;
    QString error_;
    bool attempted_ = false;
    bool terminal_ = false;
    bool ready_ = false;
    bool stopRequested_ = false;
};

} // namespace NekoGui_runtime
