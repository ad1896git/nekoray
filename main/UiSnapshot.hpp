#pragma once
#include "main/NekoGui_UiThread.hpp"
#include "main/UiOwnership.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>
namespace NekoGui_concurrency {
// Compare serialized configuration, excluding traffic, latency, report and group
// placement. Those may change while the same configuration is being tested.
template<class Profile>
QByteArray ProfileConfigFingerprint(const Profile &profile) {
    RequireUiThread();
    QJsonObject config;
    config.insert(QStringLiteral("type"), profile.type);
    config.insert(QStringLiteral("bean"), profile.bean->ToJson());
    return QJsonDocument(config).toJson(QJsonDocument::Compact);
}

template<class Profile, class Lookup, class Apply>
bool WithRegisteredConfig(int id, const std::weak_ptr<Profile> &expected,
                          const QByteArray &expectedConfig, Lookup lookup, Apply apply) {
    bool applied = false;
    WithRegistered<Profile>(id, expected, std::move(lookup),
        [&](const std::shared_ptr<Profile> &profile) {
            if (ProfileConfigFingerprint(*profile) != expectedConfig) return;
            apply(profile);
            applied = true;
        });
    return applied;
}

template<class T>
std::optional<T> ReadUi(std::function<T()> read, int timeoutMs = 5000) {
    auto *app = QCoreApplication::instance();
    if (!app) return std::nullopt;
    if (QThread::currentThread() == app->thread()) return read();
    auto value = std::make_shared<std::optional<T>>();
    const auto result = runOnUiThreadAndWait([value, read] {
        value->emplace(read());
        return UiThreadResult::Committed;
    }, timeoutMs, UiCommitRunningBudgetMs);
    if (result != UiThreadResult::Committed) return std::nullopt;
    return std::move(*value);
}
} // namespace NekoGui_concurrency
