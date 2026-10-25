#include "TrafficLooper.hpp"
#include "db/Database.hpp"
#include "main/UiOwnership.hpp"
#include "main/UiSnapshot.hpp"
#include "rpc/gRPC.h"
#include "ui/mainwindow_interface.h"
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
namespace NekoGui_traffic {
TrafficLooper *trafficLooper = new TrafficLooper;
std::shared_ptr<const TrafficSession> TrafficLooper::MakeSession(
    const QList<std::shared_ptr<TrafficData>> &items,
    const std::shared_ptr<TrafficData> &proxy) {
    NekoGui_concurrency::RequireUiThread();
    auto session = std::make_shared<TrafficSession>();
    session->window = GetMainWindow();
    if (proxy) session->proxyTag = proxy->tag;
    for (auto it = resetIdentities_.begin(); it != resetIdentities_.end();) {
        if (it->second.traffic.expired()) it = resetIdentities_.erase(it);
        else ++it;
    }
    for (const auto &item : items) {
        if (!item || item->id < 0) continue;
        auto profile = NekoGui::profileManager->GetProfile(item->id);
        if (!profile || profile->traffic_data != item) continue;
        auto &identity = resetIdentities_[item.get()];
        if (identity.traffic.lock() != item) {
            identity = {item, std::make_shared<TrafficGeneration>()};
        }
        session->targets.append(TrafficTarget{
            item->id, item->tag, profile, item, identity.generation});
        session->tags.push_back(item->tag);
    }
    if (!session->proxyTag.empty()) session->tags.push_back(session->proxyTag);
    session->tags.push_back("bypass");
    return session;
}
void TrafficLooper::ResetTraffic(const std::shared_ptr<TrafficData> &traffic) {
    NekoGui_concurrency::RequireUiThread();
    const auto it = resetIdentities_.find(traffic.get());
    if (it != resetIdentities_.end() && it->second.traffic.lock() == traffic)
        it->second.generation->Reset();
    traffic->Reset();
}
void TrafficLooper::Start(std::shared_ptr<const TrafficSession> session) {
    QMutexLocker lock(&mutex_);
    session_ = std::move(session);
    sampleClock_.Reset();
    const auto published = session_;
    NekoGui_concurrency::PostUi([this, published] { uiSession_ = published; });
    looping.store(false);
    loop_enabled.store(true);
}
void TrafficLooper::RequestShutdown() { shutdown_.store(true); }
void TrafficLooper::CollectLocked(bool connections, bool final, bool sample) {
    if (!session_) return;
    Packet packet;
    packet.session = session_;
    packet.final = final;
    packet.save = final && sample;
    // Snapshot all targets before any query, including targets sharing a tag.
    for (const auto &target : session_->targets)
        packet.generations.push_back(target.generation->Snapshot());
    if (sample) {
        packet.intervalMs =
            sampleClock_.Sample(std::chrono::steady_clock::now());
    }
#ifndef NKR_NO_GRPC
    if (sample) {
        packet.deltas = QueryUniqueTags(session_->tags,
            [](const std::string &tag, const char *direction) {
                return NekoGui_rpc::defaultClient->QueryStats(tag, direction);
            });
    }
    if (connections && !final) {
        const auto json = NekoGui_rpc::defaultClient->ListConnections();
        packet.connections = QJsonDocument::fromJson(QByteArray::fromStdString(json)).array();
        packet.showConnections = true;
    }
#else
    Q_UNUSED(connections);
#endif
    // The worker-only mutex preserves publication order. UI never acquires it.
    NekoGui_concurrency::PostUi([this, packet] { Apply(packet); });
}
void TrafficLooper::Stop(bool sampleFinal) {
    QMutexLocker lock(&mutex_);
    loop_enabled.store(false);
    CollectLocked(false, true, sampleFinal);
    session_.reset();
    looping.store(false);
}
void TrafficLooper::Apply(const Packet &packet) {
    NekoGui_concurrency::RequireUiThread();
    QSet<TrafficData *> updated;
    size_t index = 0;
    for (const auto &target : packet.session->targets) {
        const auto generation = packet.generations.at(index++);
        if (!target.generation->Accepts(generation)) continue;
        NekoGui_concurrency::WithRegistered<NekoGui::ProxyEntity>(
            target.id, target.profile,
            [](int id) { return NekoGui::profileManager->GetProfile(id); },
            [&](const std::shared_ptr<NekoGui::ProxyEntity> &profile) {
                auto traffic = target.traffic.lock();
                if (!traffic || profile->traffic_data != traffic || updated.contains(traffic.get())) return;
                updated.insert(traffic.get());
                const auto delta = packet.deltas.find(target.tag);
                if (delta != packet.deltas.end()) ApplyTrafficDelta(*traffic, delta->second, packet.intervalMs);
                if (packet.save) profile->Save();
                if (packet.session->window) packet.session->window->refresh_proxy_list(target.id);
            });
    }
    if (uiSession_ != packet.session) return;
    auto *window = packet.session->window.data();
    if (!window) return;
    if (packet.final) {
        uiSession_.reset();
        window->refresh_status("STOP");
        return;
    }
    const auto proxy = packet.deltas.find(packet.session->proxyTag);
    const auto bypass = packet.deltas.find("bypass");
    if (proxy != packet.deltas.end() && bypass != packet.deltas.end()) {
        TrafficData proxyDisplay("proxy"), bypassDisplay("bypass");
        ApplyTrafficDelta(proxyDisplay, proxy->second, packet.intervalMs);
        ApplyTrafficDelta(bypassDisplay, bypass->second, packet.intervalMs);
        window->refresh_status(QObject::tr("Proxy: %1\nDirect: %2")
            .arg(proxyDisplay.DisplaySpeed(), bypassDisplay.DisplaySpeed()));
    }
    if (packet.showConnections && NekoGui::dataStore->connection_statistics)
        window->refresh_connection_list(packet.connections);
}
void TrafficLooper::Loop() {
    while (!shutdown_.load()) {
        const auto options = NekoGui_concurrency::ReadUi<Options>([] {
            Options value;
            value.interval = NekoGui::dataStore->traffic_loop_interval;
            value.connections = NekoGui::dataStore->connection_statistics;
            return value;
        });
        if (!options) { QThread::msleep(100); continue; }
        int sleepMs = options->interval;
        if (sleepMs < 500 || sleepMs > 5000) sleepMs = 1000;
        QThread::msleep(static_cast<unsigned long>(sleepMs));
        if (shutdown_.load() || options->interval == 0) continue;
        QMutexLocker lock(&mutex_);
        if (!loop_enabled.load() || !session_) continue;
        looping.store(true);
        CollectLocked(options->connections, false, true);
    }
}
} // namespace NekoGui_traffic
