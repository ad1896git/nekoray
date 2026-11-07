// Compile the real entry point, including signal and native quit registration.
// The startup probe only arranges pending operations; it never supplies cleanup.
#define main NkrProductionMain
#include "../main/main.cpp"
#undef main

#include "ui/mainwindow.h"
#include "ui/widget/MessageBoxTimer.h"
#include <cstdio>

static void installProbe() {
    auto *poll = new QTimer(qApp);
    QObject::connect(poll, &QTimer::timeout, poll, [poll] {
        auto *window = GetMainWindow();
        if (!window || !NekoGui::dataStore->core_running) return;
        const auto mode = qEnvironmentVariable("NKR_QUIT_TEST_MODE");
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
