#pragma once

#include <chrono>
#include <optional>

namespace NekoGui_traffic {

class TrafficSampleClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Reset() {
        previous_.reset();
    }

    std::optional<long long> Sample(TimePoint now) {
        const auto previous = previous_;
        previous_ = now;
        if (!previous) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now - *previous).count();
    }

private:
    std::optional<TimePoint> previous_;
};

} // namespace NekoGui_traffic
