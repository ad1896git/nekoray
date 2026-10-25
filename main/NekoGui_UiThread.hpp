#pragma once

#include <functional>

class QObject;

enum class UiThreadResult {
    Committed,
    NotCommitted,
    Failed
};

// Subscription commit configuration for the PR-4 consumer.
inline constexpr int UiCommitQueueTimeoutMs = 10 * 1000;
// Monitored responsiveness target, not an absolute wall-clock guarantee.
inline constexpr int UiCommitRunningBudgetMs = 200;
// PR-4 must show "update cancelled" for NotCommitted, without automatic retry.

// Call once on the application thread, before starting workers.
// The context must remain alive until shutdownUiThreadDispatcher().
// Call shutdown before destruction of the context's derived-class members.
void initializeUiThreadDispatcher(QObject *context);

// Idempotent. Rejects new work and cancels pending work.
// Does not interrupt or wait for a running callback.
void shutdownUiThreadDispatcher();

// Worker-thread only. queueTimeoutMs must be >= 0;
// runningBudgetMs must be > 0.
//
// The callback must own its inputs, avoid nested event loops, and perform
// its own rollback before returning Failed or propagating an exception.
// A running callback is never abandoned when the queue timeout expires.
//
// runningBudgetMs is a diagnostic budget, not forced cancellation.
[[nodiscard]] UiThreadResult runOnUiThreadAndWait(
    std::function<UiThreadResult()> callback,
    int queueTimeoutMs,
    int runningBudgetMs);
