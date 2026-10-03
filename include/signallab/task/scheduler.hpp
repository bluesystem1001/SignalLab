#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "signallab/task/task.hpp"
#include "signallab/task/thread_pool.hpp"

namespace signallab::task {

[[nodiscard]] std::size_t calculateDefaultComputeThreadCount(
    unsigned hardwareThreadCount) noexcept;

[[nodiscard]] std::size_t defaultComputeThreadCount() noexcept;

struct SchedulerConfig {
    std::size_t computeThreadCount{defaultComputeThreadCount()};
    std::size_t ioThreadCount{1};
    std::chrono::milliseconds progressInterval{50};
};

// Scheduler owns task state and routes work to the compute or I/O pool.
// Observers may run on a worker thread and are never called while an internal
// lock is held. A GUI adapter must forward them through a queued connection.
class Scheduler {
public:
    explicit Scheduler(SchedulerConfig config = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    [[nodiscard]] core::Result<TaskId> submit(TaskSpec spec);
    [[nodiscard]] core::Result<TaskId> retry(TaskId finishedTaskId);
    [[nodiscard]] bool cancel(TaskId id);

    // Removes a task from the scheduler's in-memory history. Only terminal
    // tasks can be removed; an unknown or active task returns false. Its
    // terminal observer notification is still delivered exactly as if the
    // task remained visible, so service adapters can finish their cleanup.
    [[nodiscard]] bool remove(TaskId id);

    [[nodiscard]] std::optional<TaskSnapshot> snapshot(TaskId id) const;
    [[nodiscard]] std::vector<TaskSnapshot> snapshots() const;

    [[nodiscard]] bool waitForTerminal(
        TaskId id,
        std::chrono::milliseconds timeout) const;

    [[nodiscard]] bool waitForIdle(
        std::chrono::milliseconds timeout) const;

    // Stops acceptance and requests cancellation. When an observer callback
    // is active (including on another thread), this call only initiates
    // shutdown so that an observer may delegate it and wait without creating
    // a join cycle. A later call outside an observer, or destruction, joins
    // the worker pools.
    void shutdown();

    // Initiates shutdown and joins both worker pools. Call this from the
    // owning thread after observer delivery has been disconnected; task and
    // observer callbacks must use shutdown() to avoid joining themselves.
    void shutdownAndWait();

    [[nodiscard]] std::size_t computeThreadCount() const noexcept;
    [[nodiscard]] std::size_t ioThreadCount() const noexcept;

private:
    struct TaskRecord;

    [[nodiscard]] core::Result<TaskId> submitWithRetrySource(
        TaskSpec spec,
        TaskId retryOf);

    void runTask(const std::shared_ptr<TaskRecord>& record);
    void reportProgress(
        const std::shared_ptr<TaskRecord>& record,
        int percent,
        std::string message);
    void finishTask(
        const std::shared_ptr<TaskRecord>& record,
        std::optional<core::Error> error);
    void cancelDrainedTasks(std::vector<QueuedTask> tasks);
    void initiateShutdown();

    void notifyObserver(
        const std::shared_ptr<TaskRecord>& record,
        const TaskSnapshot& snapshot) noexcept;

    SchedulerConfig config_;
    ThreadPool computePool_;
    ThreadPool ioPool_;

    mutable std::mutex mutex_;
    mutable std::condition_variable stateChanged_;
    std::unordered_map<TaskId, std::shared_ptr<TaskRecord>> tasks_;
    TaskId nextId_{1};
    bool accepting_{true};

    // Public observer callbacks never run while a Scheduler lock is held.
    // This counter also makes callback-originated shutdown requests visible
    // when an observer delegates shutdown() to another thread.
    std::atomic<std::size_t> activeObserverCallbacks_{0};

    std::mutex shutdownInitiationMutex_;
    std::mutex shutdownMutex_;
    std::condition_variable shutdownChanged_;
    std::thread::id shutdownOwnerThread_;
    bool shutdownInProgress_{false};
    bool shutdownComplete_{false};
};

}  // namespace signallab::task
