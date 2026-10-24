#pragma once
#include "TrafficData.hpp"
#include "TrafficBatch.hpp"
#include "TrafficSampleClock.hpp"
#include <QJsonArray>
#include <QList>
#include <QMutex>
#include <QPointer>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
class MainWindow;
namespace NekoGui { class ProxyEntity; }
namespace NekoGui_traffic {
struct TrafficTarget {
    int id = -1;
    std::string tag;
    std::weak_ptr<NekoGui::ProxyEntity> profile;
    std::weak_ptr<TrafficData> traffic;
    std::shared_ptr<TrafficGeneration> generation;
};
struct TrafficSession {
    QList<TrafficTarget> targets;
    std::vector<std::string> tags;
    std::string proxyTag;
    QPointer<MainWindow> window;
};
class TrafficLooper {
public:
    std::atomic<bool> loop_enabled{false};
    std::atomic<bool> looping{false};
    // UI thread only.
    std::shared_ptr<const TrafficSession> MakeSession(
        const QList<std::shared_ptr<TrafficData>> &items,
        const std::shared_ptr<TrafficData> &proxy);
    void ResetTraffic(const std::shared_ptr<TrafficData> &traffic);
    // Worker threads only.
    void Start(std::shared_ptr<const TrafficSession> session);
    void Stop(bool sampleFinal);
    void Loop();
    // Any thread; does not wait for an outstanding RPC.
    void RequestShutdown();
private:
    struct Options { int interval = 1000; bool connections = false; };
    struct Packet {
        std::shared_ptr<const TrafficSession> session;
        TrafficDeltas deltas;
        QJsonArray connections;
        std::vector<unsigned long long> generations;
        std::optional<long long> intervalMs;
        bool showConnections = false;
        bool final = false;
        bool save = false;
    };
    void CollectLocked(bool connections, bool final, bool sample);
    void Apply(const Packet &packet);
    QMutex mutex_;
    std::shared_ptr<const TrafficSession> session_;
    TrafficSampleClock sampleClock_;
    std::atomic<bool> shutdown_{false};
    // Accessed exclusively on the UI thread.
    std::shared_ptr<const TrafficSession> uiSession_;
    struct ResetIdentity {
        std::weak_ptr<TrafficData> traffic;
        std::shared_ptr<TrafficGeneration> generation;
    };
    std::map<TrafficData *, ResetIdentity> resetIdentities_;
};
extern TrafficLooper *trafficLooper;
} // namespace NekoGui_traffic
