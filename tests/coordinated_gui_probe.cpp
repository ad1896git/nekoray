// Compile the real entry point, including signal and native quit registration.
// The probe arranges pending operations and fake launchers; cleanup is production code.
#include "ui/mainwindow.h"

#define main NkrProductionMain
#include "../main/main.cpp"
#undef main

#include "ui/widget/MessageBoxTimer.h"
#include <cstdio>

#ifdef Q_OS_MACOS
#include "sys/VpnProcess.hpp"
#include "sys/ExternalTempFiles.hpp"
#include "main/ShutdownDialogs.hpp"
#include <QScopedValueRollback>

// Substitute only an unprivileged launcher/readiness probe. Ownership, exit
// classification, stop, Tun setter, reset and shutdown all remain production.
struct MacVpnShutdownProbe {
    static void attachReady(MainWindow *window, int code) {
        const auto root = qEnvironmentVariable("NKR_QUIT_TEST_ROOT");
        auto files = std::make_shared<NekoGui_sys::ExternalTempFiles>(root);
        QString config, error;
        if (!files->write("stop-probe.json", "{}", config, error))
            qFatal("stop probe config: %s", qPrintable(error));
        auto probes = std::make_shared<int>(0);
        auto *process = new NekoGui_runtime::VpnProcess(
            [probes] { return ++*probes > 1; }, window);
        process->setCaptureStandardError(true);
        window->AttachMacVPNProcess(process, files);
        QEventLoop ready;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(process, &NekoGui_runtime::VpnProcess::ready,
                         &ready, &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &ready, &QEventLoop::quit);
        const QString script =
            "while [ -f \"$1\" ]; do /bin/sleep 0.01; done; "
            "if [ \"$2\" != 0 ]; then "
            "echo 'execution error: core did not exit; state kept for cleanup (74)' >&2; fi; "
            "exit \"$2\"";
        if (!process->start("/bin/sh", {"-c", script, "stop-probe", config,
                                         QString::number(code)}))
            qFatal("stop probe launcher failed");
        window->vpn_pid = process->processId();
        deadline.start(3000);
        ready.exec();
        if (!process->isReady()) qFatal("stop probe was never ready");
        NekoGui::dataStore->vpn_internal_tun = false;
        NekoGui::dataStore->spmode_vpn = true;
    }

    static void runStartRecovery(MainWindow *window) {
        QScopedValueRollback<bool> dialogs(ShutdownDialogs::suppressed, true);
        const auto root = qEnvironmentVariable("NKR_QUIT_TEST_ROOT");
        // Strict JSON avoids the bundled template's comments; no real Tun is made.
        QDir().mkpath("vpn");
        QFile config("vpn/sing-box-vpn.json");
        const QByteArray json = R"({"inbounds":[{"type":"tun","interface_name":"utun250"}]})";
        if (!config.open(QIODevice::WriteOnly) || config.write(json) != json.size())
            qFatal("cannot write start probe config");
        config.close();
        const auto launcher = root + "/recovery-launcher";
        const auto invoked = root + "/helper-invoked";
        const auto writeLauncher = [&](const QByteArray &ending) {
            QFile file(launcher);
            const QByteArray script = "#!/bin/sh\n/bin/cat >/dev/null\n"
                "touch \"" + invoked.toUtf8() + "\"\n" + ending;
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                file.write(script) != script.size()) qFatal("cannot write fake helper");
            file.close();
            if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
                qFatal("cannot make fake helper executable");
        };
        attachReady(window, 74);
        if (window->StopVPNProcess()) qFatal("fake failed stop returned success");
        const auto pending = window->macVpnStopFailed_;
        if (!pending || !*pending) qFatal("failed stop did not record recovery");
        const auto assertPending = [&] {
            if (window->macVpnStopFailed_ != pending || !*pending ||
                window->vpnProcess_ || window->macVpnFiles_ || window->vpn_pid ||
                !NekoGui::dataStore->spmode_vpn)
                qFatal("new Start lost pending failed-stop recovery");
        };
        // Missing helper, error and cancellation must all be refused BEFORE launch.
        if (window->StartMacVPNProcess(root + "/missing-helper"))
            qFatal("Start accepted while recovery was pending");
        assertPending();
        for (const auto &ending : {
                 QByteArray("echo 'execution error: cleanup failed (74)' >&2\nexit 1\n"),
                 QByteArray("echo 'execution error: User canceled. (-128)' >&2\nexit 1\n")}) {
            writeLauncher(ending);
            if (window->StartMacVPNProcess(launcher))
                qFatal("Start accepted while recovery was pending");
            assertPending();
            if (QFile::exists(invoked)) qFatal("pending Start invoked authorization helper");
            if (window->ResetMacVPNState(launcher)) qFatal("failed/cancelled reset succeeded");
            if (!QFile::remove(invoked)) qFatal("reset did not invoke fake helper");
            assertPending();
            if (window->StartMacVPNProcess(launcher)) qFatal("Start accepted after failed reset");
            assertPending();
            if (QFile::exists(invoked)) qFatal("retry invoked authorization helper");
        }
        writeLauncher("exit 0\n");
        if (!window->ResetMacVPNState(launcher) || window->macVpnStopFailed_)
            qFatal("successful reset did not release recovery");
        window->neko_set_spmode_vpn(false, false);
        if (NekoGui::dataStore->spmode_vpn || !window->StopVPNProcess())
            qFatal("ordinary stop did not recover after reset");
        QFile::remove(invoked);
        // The same real Start path can launch again once recovery succeeds.
        if (!window->StartMacVPNProcess(launcher)) qFatal("Start remained blocked after reset");
        QEventLoop ended;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(window->vpnProcess_, &NekoGui_runtime::VpnProcess::ended,
                         &ended, &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &ended, &QEventLoop::quit);
        deadline.start(3000);
        ended.exec();
        if (window->vpnProcess_ || !QFile::exists(invoked))
            qFatal("recovered Start did not execute fake helper");
        attachReady(window, 0);
        window->neko_set_spmode_vpn(false, false);
        if (NekoGui::dataStore->spmode_vpn || !window->StopVPNProcess())
            qFatal("normal start/stop failed after recovery");
        std::puts("MAC_START_RECOVERY_OK");
    }

    static void run(MainWindow *window) {
        attachReady(window, 74);
        bool stopped;
        {
            QScopedValueRollback<bool> dialogs(ShutdownDialogs::suppressed, true);
            stopped = window->StopVPNProcess();
        }
        const bool released = !window->vpnProcess_ && !window->macVpnFiles_ &&
                              window->vpn_pid == 0;
        const bool tunAfterStop = NekoGui::dataStore->spmode_vpn;
        attachReady(window, 74);
        const bool shutdown = window->coordinatedShutdown();
        const bool tunAfterShutdown = NekoGui::dataStore->spmode_vpn;
        std::printf("MAC_STOP_PROBE stop=%d released=%d tun_after_stop=%d "
                    "shutdown=%d tun_after_shutdown=%d dialogs_restored=%d\n",
                    stopped, released, tunAfterStop, shutdown, tunAfterShutdown,
                    !ShutdownDialogs::suppressed);
        std::fflush(stdout);
        if (stopped || !released || !tunAfterStop || shutdown ||
            !tunAfterShutdown || ShutdownDialogs::suppressed)
            qFatal("failed stop must keep Tun on and refuse shutdown");

        // Run the real troubleshooting reset with fake failure, cancel and success.
        // No AppleScript/authorization or real root state is touched.
        const auto launcher = qEnvironmentVariable("NKR_QUIT_TEST_ROOT") + "/reset-launcher";
        const auto writeLauncher = [&launcher](const QByteArray &ending) {
            QFile file(launcher);
            const QByteArray script = "#!/bin/sh\n/bin/cat >/dev/null\n" + ending;
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                file.write(script) != script.size())
                qFatal("cannot write reset launcher");
            file.close();
            if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
                qFatal("cannot make reset launcher executable");
        };
        {
            QScopedValueRollback<bool> dialogs(ShutdownDialogs::suppressed, true);
            for (const auto &ending : {
                     QByteArray("echo 'execution error: cleanup failed (74)' >&2\nexit 1\n"),
                     QByteArray("echo 'execution error: User canceled. (-128)' >&2\nexit 1\n")}) {
                writeLauncher(ending);
                if (window->ResetMacVPNState(launcher) ||
                    !NekoGui::dataStore->spmode_vpn ||
                    !window->macVpnStopFailed_ || !*window->macVpnStopFailed_)
                    qFatal("failed/cancelled reset lost pending Tun state");
            }
            writeLauncher("exit 0\n");
            if (!window->ResetMacVPNState(launcher)) qFatal("reset failed");
            // Fail here before any default launcher could request authorization.
            if (window->macVpnStopFailed_ && *window->macVpnStopFailed_)
                qFatal("successful reset retained failed-stop state");
            // Reset success must release the failure latch; this ordinary stop
            // must neither prompt again nor leave Tun permanently marked on.
            window->neko_set_spmode_vpn(false, false);
        }
        if (NekoGui::dataStore->spmode_vpn) qFatal("Tun stuck on after reset");
        attachReady(window, 0);
        {
            QScopedValueRollback<bool> dialogs(ShutdownDialogs::suppressed, true);
            window->neko_set_spmode_vpn(false, false);
        }
        if (NekoGui::dataStore->spmode_vpn) qFatal("normal stop failed");
        std::puts("MAC_STOP_RECOVERY_OK");
    }
};
#endif

static void installProbe() {
    auto *poll = new QTimer(qApp);
    QObject::connect(poll, &QTimer::timeout, poll, [poll] {
        auto *window = GetMainWindow();
        if (!window || !NekoGui::dataStore->core_running) return;
        const auto mode = qEnvironmentVariable("NKR_QUIT_TEST_MODE");
#ifdef Q_OS_MACOS
        if (mode == "mac_start_recovery") {
            poll->stop();
            MacVpnShutdownProbe::runStartRecovery(window);
            std::puts("QUIT_PROBE_READY");
            std::fflush(stdout);
            return;
        }
        if (mode == "mac_stop_failure") {
            poll->stop();
            MacVpnShutdownProbe::run(window);
            std::puts("QUIT_PROBE_READY");
            std::fflush(stdout);
            return;
        }
#endif
        if (mode == "pending_stop") {
            if (NekoGui::dataStore->started_id < 0) return;
            window->neko_stop(false, true);
        }
        if (mode != "idle") {
            bool found = false;
            for (auto *child : window->children()) {
                if (auto *timer = dynamic_cast<MessageBoxTimer *>(child)) {
                    // Make the real five-second deadline expire inside the
                    // core wait, without a five-second delay in every CI case.
                    if (timer->isActive()) {
                        timer->start(150);
                        found = true;
                    }
                }
            }
            if (!found) return;
        }
        if (mode != "idle") {
            QTimer::singleShot(75, qApp, [] {
                // Same helper as queued profile and TUN cleanup warnings.
                MessageBoxWarning("shutdown probe", "pending cleanup warning");
                MessageBoxInfo("shutdown probe", "pending cleanup information");
            });
        }
        poll->stop();
        std::puts("QUIT_PROBE_READY");
        std::fflush(stdout);
    });
    poll->start(10);
}
Q_COREAPP_STARTUP_FUNCTION(installProbe)

int main(int argc, char **argv) { return NkrProductionMain(argc, argv); }
