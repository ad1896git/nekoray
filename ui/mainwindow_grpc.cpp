#include "./ui_mainwindow.h"
#include "mainwindow.h"
#include "StopTrafficPersistence.hpp"

#include "db/Database.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/traffic/TrafficLooper.hpp"
#include "rpc/gRPC.h"
#include "sys/ExternalTempFiles.hpp"
#include <utility>
#include "ui/widget/MessageBoxTimer.h"

#include <QTimer>
#include <QThread>
#include <QInputDialog>
#include <QPushButton>
#include <QDesktopServices>
#include <QMessageBox>
#include <QDialogButtonBox>

#include "rpc/W04Support.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <list>
#include <memory>


// ext core

std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> CreateExtCFromExtR(const std::list<std::shared_ptr<NekoGui_fmt::ExternalBuildResult>> &extRs, bool start) {
    // plz run and start in same thread
    std::list<std::shared_ptr<NekoGui_sys::ExternalProcess>> l;
    for (const auto &extR: extRs) {
        std::shared_ptr<NekoGui_sys::ExternalProcess> extC(new NekoGui_sys::ExternalProcess());
        extC->tag = extR->tag;
        extC->program = extR->program;
        extC->arguments = extR->arguments;
        extC->env = extR->env;
        NekoGui_sys::AttachExternalTempFiles(
            *extC, std::move(extR->tempFiles));
        l.emplace_back(extC);
        //
        if (start) extC->Start();
    }
    return l;
}

// grpc

#ifndef NKR_NO_GRPC
using namespace NekoGui_rpc;
#endif

void MainWindow::setup_grpc() {
#ifndef NKR_NO_GRPC
    // Setup Connection
    defaultClient = new Client(
        [=](const QString &errStr) {
            MW_show_log("[Error] gRPC: " + errStr);
        },
        "127.0.0.1:" + Int2String(NekoGui::dataStore->core_port), NekoGui::dataStore->core_token);

    // Looper
    runOnNewThread([=] { NekoGui_traffic::trafficLooper->Loop(); });
#endif
}

// 测速

namespace {
NkrW04::BatchGate speedtestGate;
}

void MainWindow::speedtest_current_group(int mode, bool test_group) {
    if (speedtestGate.dispatchStop(mode)) {
        if (speedtestGate.phase() == NkrW04::Phase::Cancelling) {
            MW_show_log(QObject::tr("Cancelling speedtest..."));
        }
        return;
    }

    if (speedtestGate.phase() != NkrW04::Phase::Idle) {
        MessageBoxWarning(
            software_name,
            QObject::tr("The last speed test did not exit completely, "
                        "please wait. Stopping: waiting for the core process."));
        return;
    }

    auto group = NekoGui::profileManager->CurrentGroup();
    if (group == nullptr || group->archive) return;

    auto profiles = test_group ? group->ProfilesWithOrder()
                               : get_selected_or_group();
    if (profiles.isEmpty()) return;

#ifndef NKR_NO_GRPC
    QStringList full_test_flags;
    if (mode == libcore::FullTest) {
        auto w = new QDialog(this);
        auto layout = new QVBoxLayout(w);
        w->setWindowTitle(tr("Test Options"));
        //
        auto l1 = new QCheckBox(tr("Latency"));
        auto l2 = new QCheckBox(tr("UDP latency"));
        auto l3 = new QCheckBox(tr("Download speed"));
        auto l4 = new QCheckBox(tr("In and Out IP"));
        //
        auto box = new QDialogButtonBox;
        box->setOrientation(Qt::Horizontal);
        box->setStandardButtons(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
        connect(box, &QDialogButtonBox::accepted, w, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, w, &QDialog::reject);
        //
        layout->addWidget(l1);
        layout->addWidget(l2);
        layout->addWidget(l3);
        layout->addWidget(l4);
        layout->addWidget(box);
        if (w->exec() != QDialog::Accepted) {
            w->deleteLater();
            return;
        }
        //
        if (l1->isChecked()) full_test_flags << "1";
        if (l2->isChecked()) full_test_flags << "2";
        if (l3->isChecked()) full_test_flags << "3";
        if (l4->isChecked()) full_test_flags << "4";
        //
        w->deleteLater();
        if (full_test_flags.isEmpty()) return;
    }
    // The options dialog runs a nested event loop: check admission again.
    const auto batch = speedtestGate.begin();
    if (!batch) {
        MessageBoxWarning(
            software_name,
            QObject::tr("The last speed test did not exit completely, "
                        "please wait. Stopping: waiting for the core process."));
        return;
    }

    const int concurrency = NekoGui::dataStore->test_concurrent;
    const QString latencyUrl = NekoGui::dataStore->test_latency_url;
    const QString downloadUrl = NekoGui::dataStore->test_download_url;
    const int downloadSeconds = NekoGui::dataStore->test_download_timeout;
    const QPointer<MainWindow> window(this);

    // Queued publication owns its data. It neither captures a worker stack
    // nor keeps a destroyed MainWindow alive.
    const auto publish =
        [window, batch](const std::shared_ptr<NekoGui::ProxyEntity> &profile,
                        bool changeLatency, int latency,
                        const QString &report, const QString &error) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [window, batch, profile, changeLatency, latency, report, error] {
                    if (!window || batch->speedtest_cancel.load() ||
                        !speedtestGate.isLatest(batch)) {
                        return;
                    }

                    // Do not save a profile deleted while the test was running.
                    if (NekoGui::profileManager->GetProfile(profile->id) != profile) {
                        return;
                    }

                    if (changeLatency) profile->latency = latency;
                    profile->full_test_report = report;
                    profile->Save();

                    if (!error.isEmpty()) {
                        MW_show_log(
                            QObject::tr("[%1] test error: %2")
                                .arg(profile->bean->DisplayTypeAndName(), error));
                    }
                    window->refresh_proxy_list(profile->id);
                },
                Qt::QueuedConnection);
        };

    try {
        runOnNewThread(
            [batch, profiles, mode, full_test_flags, concurrency,
             latencyUrl, downloadUrl, downloadSeconds, publish] {
                NkrW04::Finally finished([batch] {
                    if (speedtestGate.finish(batch)) {
                        MW_show_log(QObject::tr("Speedtest finished."));
                    } else {
                        MW_show_log(QObject::tr("Stopping: waiting for the core process"));
                    }
                });

                const bool workersOK = NkrW04::runBatch(
                    concurrency, static_cast<int>(profiles.size()), batch,
                    [&](int index) {
                        if (batch->speedtest_cancel.load()) return;
                        const auto profile = profiles.at(index);

                        libcore::TestReq req;
                        req.set_mode(static_cast<libcore::TestMode>(mode));
                        req.set_timeout(10 * 1000);
                        req.set_url(latencyUrl.toStdString());

                        std::shared_ptr<NkrW04::OwnerLease> external;
                        NkrW04::Finally cleanup([&external] {
                            if (external &&
                                !external->stop(NkrW04::ExternalWaitMs)) {
                                MW_show_log(
                                    QObject::tr("Speedtest external cleanup "
                                                "is pending on the core thread."));
                            }
                        });

                        if (mode == libcore::UrlTest ||
                            mode == libcore::FullTest) {
                            const auto config = BuildConfig(profile, true, false);
                            if (!config->error.isEmpty()) {
                                publish(profile, false, 0, config->error, {});
                                return;
                            }
                            if (batch->speedtest_cancel.load()) return;

                            if (!config->extRs.empty()) {
                                using Processes =
                                    std::list<std::shared_ptr<
                                        NekoGui_sys::ExternalProcess>>;
                                const auto processes =
                                    std::make_shared<Processes>();
                                const auto results = config->extRs;

                                external =
                                    std::make_shared<NkrW04::OwnerLease>(
                                        [](NkrW04::OwnerLease::Task task) {
                                            runOnUiThread(task, DS_cores);
                                        },
                                        [](NkrW04::OwnerLease::Task task) {
                                            QTimer::singleShot(
                                                NkrW04::ExternalWarmupMs,
                                                std::move(task));
                                        },
                                        [processes, results] {
                                            // Retain all objects before starting.
                                            *processes =
                                                CreateExtCFromExtR(results, false);
                                            for (const auto &process : *processes) {
                                                process->Start();
                                            }
                                        },
                                        [processes] {
                                            for (const auto &process : *processes) {
                                                process->Kill();
                                            }
                                            // Destruction stays on DS_cores.
                                            processes->clear();
                                        },
                                        [batch] {
                                            if (speedtestGate.releaseExternal(batch)) {
                                                MW_show_log(QObject::tr("Speedtest finished."));
                                            }
                                        });
                                speedtestGate.retainExternal(batch);

                                if (!external->start(NkrW04::ExternalWaitMs)) {
                                    publish(
                                        profile, true, -1, {},
                                        QObject::tr("External test startup timed out."));
                                    return;
                                }
                            }

                            req.mutable_config()->set_core_config(
                                QJsonObject2QString(config->coreConfig, false)
                                    .toStdString());
                            req.set_in_address(
                                profile->bean->serverAddress.toStdString());
                            req.set_full_latency(full_test_flags.contains("1"));
                            req.set_full_udp_latency(full_test_flags.contains("2"));
                            req.set_full_speed(full_test_flags.contains("3"));
                            req.set_full_in_out(full_test_flags.contains("4"));
                            req.set_full_speed_url(downloadUrl.toStdString());
                            req.set_full_speed_timeout(downloadSeconds);
                        } else if (mode == libcore::TcpPing) {
                            req.set_address(
                                profile->bean->DisplayAddress().toStdString());
                        }

                        if (batch->speedtest_cancel.load()) return;

                        bool rpcOK = false;
                        const auto result = defaultClient->Test(&rpcOK, req);

                        // Returning ends this task, not the worker accounting.
                        // The cleanup guard runs on every exit.
                        if (!rpcOK || batch->speedtest_cancel.load()) return;

                        int latency = -1;
                        if (result.error().empty()) {
                            latency = result.ms();
                            if (latency == 0) latency = 1;
                        }
                        publish(
                            profile, true, latency,
                            QString::fromStdString(result.full_report()),
                            QString::fromStdString(result.error()));
                    });

                if (!workersOK) {
                    MW_show_log(
                        QObject::tr("Speedtest cancelled after a worker failure."));
                }
            });
    } catch (...) {
        // Failure to launch the controller must not leave admission closed.
        batch->speedtest_cancel.store(true);
        speedtestGate.finish(batch);
        MW_show_log(QObject::tr("Could not start speedtest workers."));
    }

#endif
}

void MainWindow::speedtest_current() {
#ifndef NKR_NO_GRPC
    last_test_time = QTime::currentTime();
    ui->label_running->setText(tr("Testing"));

    runOnNewThread([=] {
        libcore::TestReq req;
        req.set_mode(libcore::UrlTest);
        req.set_timeout(10 * 1000);
        req.set_url(NekoGui::dataStore->test_latency_url.toStdString());

        bool rpcOK;
        auto result = defaultClient->Test(&rpcOK, req);
        if (!rpcOK) return;

        auto latency = result.ms();
        last_test_time = QTime::currentTime();

        runOnUiThread([=] {
            if (!result.error().empty()) {
                MW_show_log(QStringLiteral("UrlTest error: %1").arg(result.error().c_str()));
            }
            if (latency <= 0) {
                ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
            } else if (latency > 0) {
                ui->label_running->setText(tr("Test Result") + ": " + QStringLiteral("%1 ms").arg(latency));
            }
        });
    });
#endif
}

void MainWindow::stop_core_daemon() {
#ifndef NKR_NO_GRPC
    NekoGui_rpc::defaultClient->Exit();
#endif
}

void MainWindow::neko_start(int _id) {
    if (NekoGui::dataStore->prepare_exit) return;

    auto ents = get_now_selected_list();
    auto ent = (_id < 0 && !ents.isEmpty()) ? ents.first() : NekoGui::profileManager->GetProfile(_id);
    if (ent == nullptr) return;

    if (select_mode) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }

    auto group = NekoGui::profileManager->GetGroup(ent->gid);
    if (group == nullptr || group->archive) return;

    auto result = BuildConfig(ent, false, false);
    if (!result->error.isEmpty()) {
        MessageBoxWarning("BuildConfig return error", result->error);
        return;
    }

    auto neko_start_stage2 = [=] {
#ifndef NKR_NO_GRPC
        libcore::LoadConfigReq req;
        req.set_core_config(QJsonObject2QString(result->coreConfig, false).toStdString());
        req.set_enable_nekoray_connections(NekoGui::dataStore->connection_statistics);
        if (NekoGui::dataStore->traffic_loop_interval > 0) {
            req.add_stats_outbounds("proxy");
            req.add_stats_outbounds("bypass");
        }
        //
        bool rpcOK;
        QString error = defaultClient->Start(&rpcOK, req);
        if (rpcOK && !error.isEmpty()) {
            runOnUiThread([=] { MessageBoxWarning("LoadConfig return error", error); });
            return false;
        } else if (!rpcOK) {
            return false;
        }
        //
        NekoGui_traffic::trafficLooper->proxy = result->outboundStat.get();
        NekoGui_traffic::trafficLooper->items = result->outboundStats;
        NekoGui::dataStore->ignoreConnTag = result->ignoreConnTag;
        NekoGui_traffic::trafficLooper->loop_enabled.store(true);
#endif

        runOnUiThread(
            [=] {
                auto extCs = CreateExtCFromExtR(result->extRs, true);
                NekoGui_sys::running_ext.splice(NekoGui_sys::running_ext.end(), extCs);
            },
            DS_cores);

        NekoGui::dataStore->UpdateStartedId(ent->id);
        running = ent;

        runOnUiThread([=] {
            refresh_status();
            refresh_proxy_list(ent->id);
        });

        return true;
    };

    if (!mu_starting.tryLock()) {
        MessageBoxWarning(software_name, "Another profile is starting...");
        return;
    }
    if (!mu_stopping.tryLock()) {
        MessageBoxWarning(software_name, "Another profile is stopping...");
        mu_starting.unlock();
        return;
    }
    mu_stopping.unlock();

    // check core state
    if (!NekoGui::dataStore->core_running) {
        runOnUiThread(
            [=] {
                MW_show_log("Try to start the config, but the core has not listened to the grpc port, so restart it...");
                core_process->start_profile_when_core_is_up = ent->id;
                core_process->Restart();
            },
            DS_cores);
        mu_starting.unlock();
        return; // let CoreProcess call neko_start when core is up
    }

    // timeout message
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);

    runOnNewThread([=] {
        // stop current running
        if (NekoGui::dataStore->started_id >= 0) {
            runOnUiThread([=] { neko_stop(false, true); });
            sem_stopped.acquire();
        }
        // do start
        MW_show_log(">>>>>>>> " + tr("Starting profile %1").arg(ent->bean->DisplayTypeAndName()));
        if (!neko_start_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to start profile %1").arg(ent->bean->DisplayTypeAndName()));
        }
        mu_starting.unlock();
        // cancel timeout
        runOnUiThread([=] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
#ifdef Q_OS_LINUX
            // Check systemd-resolved
            if (NekoGui::dataStore->spmode_vpn && NekoGui::dataStore->routing->direct_dns.startsWith("local") && ReadFileText("/etc/resolv.conf").contains("systemd-resolved")) {
                MW_show_log("[Warning] The default Direct DNS may not works with systemd-resolved, you may consider change your DNS settings.");
            }
#endif
        });
    });
}

void MainWindow::neko_stop(bool crash, bool sem) {
    auto id = NekoGui::dataStore->started_id;
    if (id < 0) {
        if (sem) sem_stopped.release();
        return;
    }

    auto neko_stop_stage2 = [=] {
        runOnUiThread(
            [=] {
                while (!NekoGui_sys::running_ext.empty()) {
                    auto extC = NekoGui_sys::running_ext.front();
                    extC->Kill();
                    NekoGui_sys::running_ext.pop_front();
                }
            },
            DS_cores);

#ifndef NKR_NO_GRPC
        NekoGui_traffic::trafficLooper->loop_enabled.store(false);
        NekoGui_traffic::trafficLooper->loop_mutex.lock();
        if (NekoGui::dataStore->traffic_loop_interval != 0) {
            NekoGui_traffic::trafficLooper->UpdateAll();
            NekoGui::SaveExistingTrafficProfiles(
                NekoGui_traffic::trafficLooper->items,
                [](int profileId) {
                    return NekoGui::profileManager->GetProfile(profileId);
                },
                [this](int profileId) {
                    runOnUiThread([this, profileId] {
                        refresh_proxy_list(profileId);
                    });
                });
        }
        NekoGui_traffic::trafficLooper->loop_mutex.unlock();

        if (!crash) {
            bool rpcOK;
            QString error = defaultClient->Stop(&rpcOK);
            if (rpcOK && !error.isEmpty()) {
                runOnUiThread([=] { MessageBoxWarning("Stop return error", error); });
                return false;
            } else if (!rpcOK) {
                return false;
            }
        }
#endif

        NekoGui::dataStore->UpdateStartedId(-1919);
        NekoGui::dataStore->need_keep_vpn_off = false;
        running = nullptr;

        runOnUiThread([=] {
            refresh_status();
            refresh_proxy_list(id);
        });

        return true;
    };

    if (!mu_stopping.tryLock()) {
        if (sem) sem_stopped.release();
        return;
    }

    // timeout message
    auto restartMsgbox = new QMessageBox(QMessageBox::Question, software_name, tr("If there is no response for a long time, it is recommended to restart the software."),
                                         QMessageBox::Yes | QMessageBox::No, this);
    connect(restartMsgbox, &QMessageBox::accepted, this, [=] { MW_dialog_message("", "RestartProgram"); });
    auto restartMsgboxTimer = new MessageBoxTimer(this, restartMsgbox, 5000);

    runOnNewThread([=] {
        // do stop
        MW_show_log(">>>>>>>> " + tr("Stopping profile %1").arg(running->bean->DisplayTypeAndName()));
        if (!neko_stop_stage2()) {
            MW_show_log("<<<<<<<< " + tr("Failed to stop, please restart the program."));
        }
        mu_stopping.unlock();
        if (sem) sem_stopped.release();
        // cancel timeout
        runOnUiThread([=] {
            restartMsgboxTimer->cancel();
            restartMsgboxTimer->deleteLater();
            restartMsgbox->deleteLater();
        });
    });
}

void MainWindow::CheckUpdate() {
    // on new thread...
#ifndef NKR_NO_GRPC
    bool ok;
    libcore::UpdateReq request;
    request.set_action(libcore::UpdateAction::Check);
    request.set_check_pre_release(NekoGui::dataStore->check_include_pre);
    auto response = NekoGui_rpc::defaultClient->Update(&ok, request);
    if (!ok) return;

    auto err = response.error();
    if (!err.empty()) {
        runOnUiThread([=] {
            MessageBoxWarning(QObject::tr("Update"), err.c_str());
        });
        return;
    }

    if (response.release_download_url() == nullptr) {
        runOnUiThread([=] {
            MessageBoxInfo(QObject::tr("Update"), QObject::tr("No update"));
        });
        return;
    }

    runOnUiThread([=] {
        auto allow_updater = !NekoGui::dataStore->flag_use_appdata;
        auto note_pre_release = response.is_pre_release() ? " (Pre-release)" : "";
        QMessageBox box(QMessageBox::Question, QObject::tr("Update") + note_pre_release,
                        QObject::tr("Update found: %1\nRelease note:\n%2").arg(response.assets_name().c_str(), response.release_note().c_str()));
        //
        QAbstractButton *btn1 = nullptr;
        if (allow_updater) {
            btn1 = box.addButton(QObject::tr("Update"), QMessageBox::AcceptRole);
        }
        QAbstractButton *btn2 = box.addButton(QObject::tr("Open in browser"), QMessageBox::AcceptRole);
        box.addButton(QObject::tr("Close"), QMessageBox::RejectRole);
        box.exec();
        //
        if (btn1 == box.clickedButton() && allow_updater) {
            // Download Update
            runOnNewThread([=] {
                bool ok2;
                libcore::UpdateReq request2;
                request2.set_action(libcore::UpdateAction::Download);
                auto response2 = NekoGui_rpc::defaultClient->Update(&ok2, request2);
                runOnUiThread([=] {
                    if (response2.error().empty()) {
                        auto q = QMessageBox::question(nullptr, QObject::tr("Update"),
                                                       QObject::tr("Update is ready, restart to install?"));
                        if (q == QMessageBox::StandardButton::Yes) {
                            this->exit_reason = 1;
                            on_menu_exit_triggered();
                        }
                    } else {
                        MessageBoxWarning(QObject::tr("Update"), response2.error().c_str());
                    }
                });
            });
        } else if (btn2 == box.clickedButton()) {
            QDesktopServices::openUrl(QUrl(response.release_url().c_str()));
        }
    });
#endif
}
