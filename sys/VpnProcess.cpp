#include "VpnProcess.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <utility>

namespace NekoGui_runtime {

namespace {

// Replace comments with whitespace without changing quoted strings.
// Also accepts trailing commas outside strings.
QByteArray NormalizeJson(const QByteArray &input) {
    QByteArray output = input;
    bool quoted = false;
    bool escaped = false;

    for (int i = 0; i < output.size(); ++i) {
        const char ch = output.at(i);
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            continue;
        }
        if (ch != '/' || i + 1 >= output.size()) continue;

        if (output.at(i + 1) == '/') {
            output[i++] = ' ';
            output[i] = ' ';
            while (i + 1 < output.size()
                   && output.at(i + 1) != '\n'
                   && output.at(i + 1) != '\r') {
                output[++i] = ' ';
            }
        } else if (output.at(i + 1) == '*') {
            output[i++] = ' ';
            output[i] = ' ';
            bool closed = false;
            while (++i < output.size()) {
                if (output.at(i) == '*'
                    && i + 1 < output.size()
                    && output.at(i + 1) == '/') {
                    output[i] = ' ';
                    output[++i] = ' ';
                    closed = true;
                    break;
                }
                if (output.at(i) != '\n' && output.at(i) != '\r')
                    output[i] = ' ';
            }
            if (!closed) return {};
        }
    }

    quoted = false;
    escaped = false;
    for (int i = 0; i < output.size(); ++i) {
        const char ch = output.at(i);
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            continue;
        }
        if (ch != ',') continue;

        int next = i + 1;
        while (next < output.size()
               && (output.at(next) == ' ' || output.at(next) == '\t'
                   || output.at(next) == '\r'
                   || output.at(next) == '\n')) {
            ++next;
        }
        if (next < output.size()
            && (output.at(next) == '}' || output.at(next) == ']')) {
            output[i] = ' ';
        }
    }

    return output;
}

} // namespace

QString TunInterfaceName(const QByteArray &config) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        NormalizeJson(config), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};

    QString name;
    int tunCount = 0;
    const auto inbounds =
        document.object().value(QStringLiteral("inbounds")).toArray();
    for (const auto &value : inbounds) {
        const auto inbound = value.toObject();
        if (inbound.value(QStringLiteral("type")).toString()
            != QStringLiteral("tun")) {
            continue;
        }
        ++tunCount;
        name = inbound.value(QStringLiteral("interface_name")).toString();
    }

    if (tunCount != 1 || name.isEmpty()) return {};
    return name;
}

VpnProcess::VpnProcess(InterfaceProbe interfacePresent, QObject *parent)
    : QObject(parent),
      interfacePresent_(std::move(interfacePresent)) {
    process_.setProcessChannelMode(QProcess::ForwardedChannels);
    poll_.setInterval(100);

    connect(&poll_, &QTimer::timeout,
            this, &VpnProcess::checkInterface);

    // Queue terminal delivery so start() and its caller finish assigning
    // ownership and the requested UI state before handling an early exit.
    connect(&process_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VpnProcess::finish,
            Qt::QueuedConnection);

    connect(&process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (terminal_ || error != QProcess::FailedToStart) return;
        terminal_ = true;
        poll_.stop();
        error_ = tr("Failed to start VPN launcher: %1")
                     .arg(process_.errorString());
        emit ended(error_);
    }, Qt::QueuedConnection);
}

bool VpnProcess::start(const QString &program,
                       const QStringList &arguments) {
    if (attempted_) {
        error_ = tr("This VPN launch attempt has already been used.");
        return false;
    }
    attempted_ = true;

    if (!interfacePresent_) {
        terminal_ = true;
        error_ = tr("The VPN readiness probe is unavailable.");
        return false;
    }

    // An interface that predates this attempt cannot establish readiness
    // for this launch.
    if (interfacePresent_()) {
        terminal_ = true;
        error_ = tr("The configured Tun interface already exists.");
        return false;
    }

    process_.start(program, arguments);
    if (!process_.waitForStarted(3000)) {
        error_ = tr("Failed to start VPN launcher: %1")
                     .arg(process_.errorString());
        terminal_ = true;
        if (process_.state() != QProcess::NotRunning) {
            process_.kill();
            process_.waitForFinished(1000);
        }
        return false;
    }

    poll_.start();
    return true;
}

void VpnProcess::checkInterface() {
    if (terminal_ || ready_ || stopRequested_
        || process_.state() != QProcess::Running) {
        return;
    }
    if (!interfacePresent_()) return;

    ready_ = true;
    poll_.stop();
    emit ready();
}

void VpnProcess::finish(int exitCode,
                        QProcess::ExitStatus exitStatus) {
    if (terminal_) return;
    terminal_ = true;
    poll_.stop();

    if (!stopRequested_) {
        if (!ready_) {
            error_ =
                tr("VPN launcher exited before Tun became ready "
                   "(exit code %1, %2).")
                    .arg(exitCode)
                    .arg(exitStatus == QProcess::CrashExit
                             ? tr("crashed") : tr("normal exit"));
        } else if (exitStatus == QProcess::CrashExit || exitCode != 0) {
            error_ =
                tr("VPN launcher exited after Tun became ready "
                   "(exit code %1, %2).")
                    .arg(exitCode)
                    .arg(exitStatus == QProcess::CrashExit
                             ? tr("crashed") : tr("normal exit"));
        }
    }

    emit ended(error_);
}

} // namespace NekoGui_runtime
