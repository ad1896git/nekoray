#pragma once

#include <QByteArray>
#include <QRandomGenerator>
#include <memory>
#include <mutex>
#include <utility>

namespace NkrW04 {

// An immutable token snapshot identifies one core launch. Requests retain their
// snapshot even when the same client and loopback port are reused after restart.
class RpcAuth {
public:
    using Launch = std::shared_ptr<const QByteArray>;

    explicit RpcAuth(QByteArray token = {})
        : launch_(std::make_shared<const QByteArray>(std::move(token))) {}

    Launch snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return launch_;
    }

    bool isCurrent(const Launch &launch) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return launch_ == launch;
    }

    // Serialize response publication with rotation, without holding a lock
    // across network I/O. A retired launch cannot fill a caller's response.
    template<class Publish>
    bool publish(const Launch &launch, Publish publishReply) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (launch_ != launch) return false;
        publishReply();
        return true;
    }

    Launch startLaunch() {
        // Use system entropy directly, rather than a PRNG seeded once per GUI.
        QByteArray token;
        for (int i = 0; i < 4; ++i)
            token += QByteArray::number(QRandomGenerator::system()->generate64(), 16)
                         .rightJustified(16, '0');
        auto launch = std::make_shared<const QByteArray>(std::move(token));
        std::lock_guard<std::mutex> lock(mutex_);
        launch_ = launch;
        return launch;
    }

private:
    mutable std::mutex mutex_;
    Launch launch_;
};

} // namespace NkrW04
