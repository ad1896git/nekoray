#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>
namespace NekoGui_traffic {
struct TrafficDelta { long long uplink = 0; long long downlink = 0; };
using TrafficDeltas = std::map<std::string, TrafficDelta>;
using TrafficQuery = std::function<long long(const std::string &, const char *)>;
inline TrafficDeltas QueryUniqueTags(const std::vector<std::string> &tags, const TrafficQuery &query) {
    TrafficDeltas result;
    for (const auto &tag : tags) {
        if (result.find(tag) != result.end()) continue;
        TrafficDelta delta;
        delta.uplink = query(tag, "uplink");
        delta.downlink = query(tag, "downlink");
        result.emplace(tag, delta);
    }
    return result;
}
template<class Traffic>
void ApplyTrafficDelta(Traffic &traffic, const TrafficDelta &delta, long long intervalMs) {
    const auto interval = std::max(1LL, intervalMs);
    traffic.uplink += delta.uplink;
    traffic.downlink += delta.downlink;
    traffic.uplink_rate = delta.uplink * 1000 / interval;
    traffic.downlink_rate = delta.downlink * 1000 / interval;
}
// Shared by a UI reset and worker packets. A packet records the generation
// before issuing RPCs, and may apply only while that generation is current.
class TrafficGeneration {
public:
    unsigned long long Snapshot() const { return value_.load(); }
    void Reset() { value_.fetch_add(1); }
    bool Accepts(unsigned long long value) const { return Snapshot() == value; }
private:
    std::atomic<unsigned long long> value_{0};
};
} // namespace NekoGui_traffic
