#include "SubscriptionCommit.hpp"

#include "GroupUpdater.hpp"
#include "db/ProfileFilter.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QThread>

#include <memory>
#include <utility>

namespace NekoGui_sub::Subscription {
namespace {

bool OnUiThread() {
    const auto *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread();
}

template<class F>
bool Attempt(F &&operation) {
    try {
        return operation();
    } catch (...) {
        return false;
    }
}

void Checkpoint(const Io &io, const QString &name) {
    if (io.checkpoint) io.checkpoint(name);
}

QList<int> Ids(const Entities &entities) {
    QList<int> result;
    QSet<int> seen;
    for (const auto &ent : entities) {
        if (!ent || ent->id < 0 || seen.contains(ent->id)) continue;
        seen.insert(ent->id);
        result.append(ent->id);
    }
    return result;
}

// Include omitted empty strings so restoration resets them as well.
QJsonObject Snapshot(const GroupPtr &group) {
    auto result = group->ToJson();
    for (auto it = group->_map.constBegin(); it != group->_map.constEnd(); ++it) {
        const auto &item = it.value();
        if (item && item->type == itemType::string &&
            !result.contains(item->name)) {
            result.insert(item->name, QString());
        }
    }
    return result;
}

struct Addition {
    Entity ent;
    int originalGid;
    QString originalPath;
    QByteArray originalCache;
};

} // namespace

UiThreadResult UiResult(const Reply &reply) {
    if (Succeeded(reply)) return UiThreadResult::Committed;
    if (reply.status == Status::Rejected ||
        reply.status == Status::Cancelled) {
        return UiThreadResult::NotCommitted;
    }
    return UiThreadResult::Failed;
}

bool Parse(const QString &content, Entities &staged) {
    staged.clear();
    if (!OnUiThread()) return false;

    try {
        RawUpdater parser;
        parser.update(content);
        staged = parser.updated_order;
    } catch (...) {
        staged.clear();
        return false;
    }
    return !staged.isEmpty();
}

Reply Commit(const GroupPtr &group,
             const Entities &input,
             bool subscription,
             bool clear,
             const QString &info,
             const Io &io) {
    if (!OnUiThread()) return {Status::Failed};

    if (!group || group->archive ||
        NekoGui::profileManager->GetGroup(group->id) != group ||
        input.isEmpty()) {
        return {Status::Rejected};
    }

    for (const auto &ent : input) {
        if (!ent || !ent->bean || ent->id >= 0) return {Status::Failed};
    }

    const auto snapshot = Snapshot(group);
    const Entities allBefore = group->Profiles();
    const Entities visibleBefore = group->ProfilesWithOrder();
    const QList<int> frozenOrder = Ids(visibleBefore);
    const int startedId = NekoGui::dataStore->started_id;

    Entity running = NekoGui::profileManager->GetProfile(startedId);
    if (running && running->gid != group->id) running.reset();

    Entities staged;
    if (subscription) {
        NekoGui::ProfileFilter::Uniq(input, staged, false, false);
    } else {
        staged = input;
    }

    Entities finalEntities;
    Entities toAdd;

    if (!subscription) {
        finalEntities = visibleBefore;
        if (running && !finalEntities.contains(running)) {
            finalEntities.append(running);
        }
        finalEntities.append(staged);
        toAdd = staged;
    } else {
        Entities candidates;
        if (running) candidates.append(running);

        if (!clear) {
            for (const auto &ent : visibleBefore) {
                if (!candidates.contains(ent)) candidates.append(ent);
            }
        }

        Entities matchedOld;
        Entities matchedNew;
        NekoGui::ProfileFilter::Common(
            candidates, staged, matchedOld, matchedNew, false);

        QMap<const NekoGui::ProxyEntity *, Entity> replacements;
        for (int i = 0; i < matchedNew.size(); ++i) {
            replacements.insert(matchedNew.at(i).get(), matchedOld.at(i));
        }

        for (const auto &ent : staged) {
            const auto found = replacements.constFind(ent.get());
            if (found == replacements.constEnd()) {
                finalEntities.append(ent);
                toAdd.append(ent);
            } else {
                finalEntities.append(found.value());
            }
        }

        // D2: an unmatched running profile stays visible at the end.
        if (running && !finalEntities.contains(running)) {
            finalEntities.append(running);
        }
    }

    QList<Addition> additions;
    bool pinned = false;
    bool published = false;

    const auto restore = [&] {
        group->FromJson(snapshot);
    };

    const auto retainFrozen = [&] {
        restore();
        group->order = frozenOrder;
        group->order_explicit = true;
        group->recoveryRequired = true;
        // The successful pin already persisted exactly this visible set
        // with the old metadata and a true recovery marker.
    };

    const auto rollback = [&]() -> Reply {
        bool removedAll = true;

        for (const auto &addition : additions) {
            const auto &ent = addition.ent;
            if (ent->id < 0) continue;

            if (!Attempt([&] { return io.remove(ent->id); })) {
                removedAll = false;
                continue;
            }

            ent->id = -1;
            ent->gid = addition.originalGid;
            ent->fn = addition.originalPath;
            ent->last_save_content = addition.originalCache;
        }

        if (!removedAll) {
            retainFrozen();
            return {Status::RecoveryFailed};
        }

        restore();
        if (!Attempt([&] { return io.save(group); })) {
            retainFrozen();
            return {Status::RecoveryFailed};
        }
        return {Status::Failed};
    };

    try {
        Checkpoint(io, QStringLiteral("before-pin"));

        group->order = frozenOrder;
        group->order_explicit = true;
        group->recoveryRequired = true;
        if (!Attempt([&] { return io.save(group); })) {
            restore();
            return {Status::Failed};
        }
        pinned = true;
        Checkpoint(io, QStringLiteral("pinned"));

        int addedCount = 0;
        for (const auto &ent : toAdd) {
            additions.append(
                Addition{ent, ent->gid, ent->fn, ent->last_save_content});

            if (!Attempt([&] { return io.add(ent, group->id); })) {
                return rollback();
            }
            Checkpoint(io, QStringLiteral("added-%1").arg(++addedCount));
        }

        group->order = Ids(finalEntities);
        group->order_explicit = true;
        group->recoveryRequired = true;
        if (subscription) {
            group->sub_last_update =
                QDateTime::currentMSecsSinceEpoch() / 1000;
            group->info = info;
        }

        if (!Attempt([&] { return io.save(group); })) {
            return rollback();
        }
        published = true;
        Checkpoint(io, QStringLiteral("published"));

        bool cleanupFailed = false;
        int deletedCount = 0;
        for (const auto &ent : allBefore) {
            if (group->order.contains(ent->id) || ent->id == startedId) {
                continue;
            }

            if (!Attempt([&] { return io.remove(ent->id); })) {
                cleanupFailed = true;
            }
            Checkpoint(io, QStringLiteral("deleted-%1").arg(++deletedCount));
        }

        if (cleanupFailed) return {Status::CommittedCleanupPending};

        group->recoveryRequired = false;
        if (!Attempt([&] { return io.save(group); })) {
            group->recoveryRequired = true;
            return {Status::CommittedCleanupPending};
        }

        Checkpoint(io, QStringLiteral("cleaned"));
        return {Status::Committed};
    } catch (...) {
        if (published) {
            group->recoveryRequired = true;
            Attempt([&] { return io.save(group); });
            return {Status::CommittedCleanupPending};
        }
        if (pinned) return rollback();

        restore();
        return {Status::Failed};
    }
}

Reply Apply(const GroupPtr &group,
            const QString &content,
            bool subscription,
            bool clear,
            const QString &info,
            const Io &io) {
    Entities staged;
    if (!Parse(content, staged)) return {Status::Rejected};
    return Commit(group, staged, subscription, clear, info, io);
}

Reply Dispatch(std::function<Reply()> callback,
               int queueTimeoutMs,
               int runningBudgetMs) {
    if (OnUiThread()) return {Status::Failed};

    const auto reply = std::make_shared<Reply>();
    const auto result = runOnUiThreadAndWait(
        [reply, callback = std::move(callback)] {
            *reply = callback();
            return UiResult(*reply);
        },
        queueTimeoutMs,
        runningBudgetMs);

    if (result == UiThreadResult::NotCommitted &&
        reply->status == Status::Cancelled) {
        return {Status::Cancelled};
    }
    if (result == UiThreadResult::Failed &&
        reply->status == Status::Cancelled) {
        return {Status::Failed};
    }
    return *reply;
}

} // namespace NekoGui_sub::Subscription
