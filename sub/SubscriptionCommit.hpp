#pragma once

#include "db/Database.hpp"
#include "main/NekoGui_UiThread.hpp"

#include <functional>

namespace NekoGui_sub::Subscription {

using Entity = std::shared_ptr<NekoGui::ProxyEntity>;
using Entities = QList<Entity>;
using GroupPtr = std::shared_ptr<NekoGui::Group>;

enum class Status {
    Committed,
    CommittedCleanupPending,
    Rejected,
    Cancelled,
    Failed,
    RecoveryFailed
};

struct Reply {
    Status status = Status::Cancelled;
};

inline bool Succeeded(const Reply &reply) {
    return reply.status == Status::Committed ||
           reply.status == Status::CommittedCleanupPending;
}

UiThreadResult UiResult(const Reply &reply);

// Production supplies the defaults. Overrides are per-call, never global.
// A false result means the operation did not publish its requested change.
// Overrides must not throw after successfully publishing a change.
struct Io {
    std::function<bool(const Entity &, int)> add =
        [](const Entity &ent, int gid) {
            return NekoGui::profileManager->AddProfile(ent, gid);
        };

    std::function<bool(const GroupPtr &)> save =
        [](const GroupPtr &group) {
            return group->SaveChecked().ok;
        };

    std::function<bool(int)> remove =
        [](int id) {
            return NekoGui::profileManager->DeleteProfile(id);
        };

    // May throw to test interruption between completed operations.
    std::function<void(const QString &)> checkpoint;
};

// UI-thread only: the existing parsers read UI-owned settings.
bool Parse(const QString &content, Entities &staged);

// UI-thread only. subscription=false appends and preserves duplicates.
Reply Commit(const GroupPtr &group,
             const Entities &input,
             bool subscription,
             bool clear,
             const QString &info,
             const Io &io = Io{});

Reply Apply(const GroupPtr &group,
            const QString &content,
            bool subscription,
            bool clear,
            const QString &info,
            const Io &io = Io{});

// Worker-thread only; delegates cancellation semantics to PR-6a.
Reply Dispatch(std::function<Reply()> callback,
               int queueTimeoutMs = UiCommitQueueTimeoutMs,
               int runningBudgetMs = UiCommitRunningBudgetMs);

} // namespace NekoGui_sub::Subscription
