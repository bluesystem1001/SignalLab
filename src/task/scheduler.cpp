#include "signallab/task/scheduler.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::task {

namespace {

constexpr const char* kCancelledMessage = "Task cancelled.";

}  // namespace

struct Scheduler::TaskRecord {
    TaskSnapshot snapshot;
    TaskAction action;
    TaskObserver observer;
    // The mutex protects notification ordering only. It is always released
    // before user observer code is invoked.
    std::mutex observerMutex;
    std::deque<TaskSnapshot> observerNotifications;
    bool observerDispatchActive{false};
    int lastObserverPhase{-1};
    int lastObserverProgress{-1};
    std::stop_source stopSource;
    std::chrono::steady_clock::time_point lastProgressTime{};
    bool hasReportedProgress{false};
};

TaskContext::TaskContext(
    TaskId id,
    std::stop_token stopToken,
    ProgressFunction progressFunction)
    : id_(id),
      stopToken_(stopToken),
      progressFunction_(std::move(progressFunction))
{
}

TaskId TaskContext::id() const noexcept
{
    return id_;
}

std::stop_token TaskContext::stopToken() const noexcept
{
    return stopToken_;
}

bool TaskContext::stopRequested() const noexcept
{
    return stopToken_.stop_requested();
}

void TaskContext::reportProgress(int percent, std::string message) const
{
    if (progressFunction_) {
        progressFunction_(percent, std::move(message));
    }
}

bool isTerminal(TaskStatus status) noexcept
{
    return status == TaskStatus::Succeeded
        || status == TaskStatus::Failed
        || status == TaskStatus::Cancelled;
}

std::size_t calculateDefaultComputeThreadCount(
    unsigned hardwareThreadCount) noexcept
{
    if (hardwareThreadCount <= 2U) {
        return 2;
    }
    return std::max<std::size_t>(
        2,
        static_cast<std::size_t>(hardwareThreadCount - 2U));
}

std::size_t defaultComputeThreadCount() noexcept
{
    return calculateDefaultComputeThreadCount(
        std::thread::hardware_concurrency());
}

Scheduler::Scheduler(SchedulerConfig config)
    : config_(config),
      computePool_(config.computeThreadCount),
      ioPool_(config.ioThreadCount)
{
    if (config.ioThreadCount < 1 || config.ioThreadCount > 2) {
        throw std::invalid_argument(
            "The I/O thread count must be one or two.");
    }
    if (config.progressInterval.count() < 0) {
        throw std::invalid_argument(
            "The progress interval must not be negative.");
    }
}

Scheduler::~Scheduler()
{
    shutdownAndWait();
}

core::Result<TaskId> Scheduler::submit(TaskSpec spec)
{
    return submitWithRetrySource(std::move(spec), 0);
}

core::Result<TaskId> Scheduler::retry(TaskId finishedTaskId)
{
    TaskSpec retrySpec;
    {
        std::lock_guard lock(mutex_);
        const auto item = tasks_.find(finishedTaskId);
        if (item == tasks_.end()) {
            return core::Result<TaskId>::failure(
                core::ErrorCode::InvalidParameter,
                "The task to retry does not exist.");
        }
        if (!isTerminal(item->second->snapshot.status)) {
            return core::Result<TaskId>::failure(
                core::ErrorCode::InvalidParameter,
                "Only a finished task can be retried.");
        }

        retrySpec.name = item->second->snapshot.name;
        retrySpec.lane = item->second->snapshot.lane;
        retrySpec.action = item->second->action;
        retrySpec.observer = item->second->observer;
    }

    return submitWithRetrySource(std::move(retrySpec), finishedTaskId);
}

bool Scheduler::cancel(TaskId id)
{
    std::shared_ptr<TaskRecord> record;
    TaskSnapshot cancelledSnapshot;
    bool notifyCancelled = false;

    {
        std::lock_guard lock(mutex_);
        const auto item = tasks_.find(id);
        if (item == tasks_.end() || isTerminal(item->second->snapshot.status)) {
            return false;
        }

        record = item->second;
        record->stopSource.request_stop();

        ThreadPool& pool = record->snapshot.lane == TaskLane::Compute
            ? computePool_
            : ioPool_;
        auto removed = pool.removePending(id);
        if (removed.has_value()
            && record->snapshot.status == TaskStatus::Pending) {
            record->snapshot.status = TaskStatus::Cancelled;
            record->snapshot.errorMessage = kCancelledMessage;
            cancelledSnapshot = record->snapshot;
            notifyCancelled = true;
        }
    }

    if (notifyCancelled) {
        stateChanged_.notify_all();
        notifyObserver(record, cancelledSnapshot);
    }
    return true;
}

std::optional<TaskSnapshot> Scheduler::snapshot(TaskId id) const
{
    std::lock_guard lock(mutex_);
    const auto item = tasks_.find(id);
    if (item == tasks_.end()) {
        return std::nullopt;
    }
    return item->second->snapshot;
}

std::vector<TaskSnapshot> Scheduler::snapshots() const
{
    std::vector<TaskSnapshot> result;
    {
        std::lock_guard lock(mutex_);
        result.reserve(tasks_.size());
        for (const auto& [id, record] : tasks_) {
            static_cast<void>(id);
            result.push_back(record->snapshot);
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const TaskSnapshot& left, const TaskSnapshot& right) {
            return left.id < right.id;
        });
    return result;
}

bool Scheduler::waitForTerminal(
    TaskId id,
    std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(mutex_);
    return stateChanged_.wait_for(lock, timeout, [this, id]() {
        const auto item = tasks_.find(id);
        return item != tasks_.end()
            && isTerminal(item->second->snapshot.status);
    });
}

bool Scheduler::waitForIdle(std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(mutex_);
    return stateChanged_.wait_for(lock, timeout, [this]() {
        return std::all_of(
            tasks_.begin(),
            tasks_.end(),
            [](const auto& item) {
                return isTerminal(item.second->snapshot.status);
            });
    });
}

void Scheduler::shutdown()
{
    // The counter is incremented before entering user code. A correctly
    // synchronized hand-off performed by that code therefore makes the
    // active state visible to the delegated shutdown caller. If a synchronous
    // join wins the race before the increment, a later delegated shutdown
    // still observes the active callback and returns without waiting.
    if (activeObserverCallbacks_.load(std::memory_order_acquire) != 0
        || computePool_.isCurrentThreadWorker()
        || ioPool_.isCurrentThreadWorker()) {
        // An observer may synchronously delegate shutdown() to another
        // thread and wait for it. Do not join a worker that may be executing
        // that observer. Initiate cancellation; a later ordinary call or the
        // destructor performs the joins.
        initiateShutdown();
        return;
    }

    shutdownAndWait();
}

void Scheduler::shutdownAndWait()
{
    if (computePool_.isCurrentThreadWorker()
        || ioPool_.isCurrentThreadWorker()) {
        // Keep this defensive even though the public contract requires the
        // owning thread to call this method. Destruction from a task/observer
        // would make the Scheduler lifetime invalid regardless.
        initiateShutdown();
        return;
    }

    {
        std::unique_lock shutdownLock(shutdownMutex_);
        if (shutdownComplete_) {
            return;
        }
        if (shutdownInProgress_) {
            if (shutdownOwnerThread_ == std::this_thread::get_id()) {
                return;
            }
            shutdownChanged_.wait(shutdownLock, [this]() {
                return shutdownComplete_;
            });
            return;
        }
        shutdownInProgress_ = true;
        shutdownOwnerThread_ = std::this_thread::get_id();
    }

    initiateShutdown();

    computePool_.join();
    ioPool_.join();

    {
        std::lock_guard shutdownLock(shutdownMutex_);
        shutdownComplete_ = true;
        shutdownInProgress_ = false;
        shutdownOwnerThread_ = {};
    }
    shutdownChanged_.notify_all();
    stateChanged_.notify_all();
}

void Scheduler::initiateShutdown()
{
    std::vector<std::pair<std::shared_ptr<TaskRecord>, TaskSnapshot>>
        pendingNotifications;
    std::vector<QueuedTask> computePending;
    std::vector<QueuedTask> ioPending;
    {
        std::lock_guard initiationLock(shutdownInitiationMutex_);
        {
            std::lock_guard lock(mutex_);
            accepting_ = false;
            for (const auto& [id, record] : tasks_) {
                static_cast<void>(id);
                if (!isTerminal(record->snapshot.status)) {
                    record->stopSource.request_stop();
                }
                if (record->snapshot.status == TaskStatus::Pending) {
                    record->snapshot.status = TaskStatus::Cancelled;
                    record->snapshot.errorMessage = kCancelledMessage;
                    pendingNotifications.emplace_back(
                        record,
                        record->snapshot);
                }
            }
        }

        computePending = computePool_.stopAccepting();
        ioPending = ioPool_.stopAccepting();
        computePool_.requestStop();
        ioPool_.requestStop();
    }

    stateChanged_.notify_all();
    for (const auto& [record, snapshot] : pendingNotifications) {
        notifyObserver(record, snapshot);
    }
    cancelDrainedTasks(std::move(computePending));
    cancelDrainedTasks(std::move(ioPending));
}

std::size_t Scheduler::computeThreadCount() const noexcept
{
    return computePool_.workerCount();
}

std::size_t Scheduler::ioThreadCount() const noexcept
{
    return ioPool_.workerCount();
}

core::Result<TaskId> Scheduler::submitWithRetrySource(
    TaskSpec spec,
    TaskId retryOf)
{
    if (spec.name.empty() || !spec.action) {
        return core::Result<TaskId>::failure(
            core::ErrorCode::InvalidParameter,
            "A task needs a name and an action.");
    }

    auto record = std::make_shared<TaskRecord>();
    record->action = std::move(spec.action);
    record->observer = std::move(spec.observer);
    TaskSnapshot pendingSnapshot;

    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            return core::Result<TaskId>::failure(
                core::ErrorCode::Cancelled,
                "The scheduler is shutting down and cannot accept tasks.");
        }
        if (nextId_ == std::numeric_limits<TaskId>::max()) {
            return core::Result<TaskId>::failure(
                core::ErrorCode::InvalidParameter,
                "No more task identifiers are available.");
        }

        record->snapshot.id = nextId_++;
        record->snapshot.name = std::move(spec.name);
        record->snapshot.lane = spec.lane;
        record->snapshot.retryOf = retryOf;
        tasks_.emplace(record->snapshot.id, record);
        pendingSnapshot = record->snapshot;
    }

    // Publish PENDING before making the work visible to a worker. This keeps
    // each observer's state sequence in order without calling it under the
    // scheduler mutex.
    notifyObserver(record, pendingSnapshot);

    TaskSnapshot cancelledSnapshot;
    bool cancelledBeforeQueue = false;
    {
        std::lock_guard lock(mutex_);
        if (isTerminal(record->snapshot.status)) {
            // shutdown() may have completed this still-being-submitted record.
        } else if (!accepting_ || record->stopSource.stop_requested()) {
            record->snapshot.status = TaskStatus::Cancelled;
            record->snapshot.errorMessage = kCancelledMessage;
            cancelledSnapshot = record->snapshot;
            cancelledBeforeQueue = true;
        } else {
            QueuedTask queued{
                record->snapshot.id,
                [this, record]() { runTask(record); }};
            ThreadPool& pool = spec.lane == TaskLane::Compute
                ? computePool_
                : ioPool_;
            if (!pool.submit(std::move(queued))) {
                record->snapshot.status = TaskStatus::Cancelled;
                record->snapshot.errorMessage =
                    "The selected task queue is closed.";
                cancelledSnapshot = record->snapshot;
                cancelledBeforeQueue = true;
            }
        }
    }

    if (cancelledBeforeQueue) {
        stateChanged_.notify_all();
        notifyObserver(record, cancelledSnapshot);
    }
    return core::Result<TaskId>::success(pendingSnapshot.id);
}

void Scheduler::runTask(const std::shared_ptr<TaskRecord>& record)
{
    TaskSnapshot runningSnapshot;
    bool wasCancelledBeforeStart = false;

    {
        std::lock_guard lock(mutex_);
        if (record->snapshot.status != TaskStatus::Pending) {
            return;
        }

        if (record->stopSource.stop_requested()) {
            record->snapshot.status = TaskStatus::Cancelled;
            record->snapshot.errorMessage = kCancelledMessage;
            wasCancelledBeforeStart = true;
        } else {
            record->snapshot.status = TaskStatus::Running;
        }
        runningSnapshot = record->snapshot;
    }

    stateChanged_.notify_all();
    notifyObserver(record, runningSnapshot);
    if (wasCancelledBeforeStart) {
        return;
    }

    const std::weak_ptr<TaskRecord> weakRecord = record;
    TaskContext context(
        record->snapshot.id,
        record->stopSource.get_token(),
        [this, weakRecord](int percent, std::string message) {
            if (auto activeRecord = weakRecord.lock()) {
                reportProgress(
                    activeRecord,
                    percent,
                    std::move(message));
            }
        });

    try {
        auto result = record->action(context);
        if (result) {
            finishTask(record, std::nullopt);
        } else {
            finishTask(record, result.error());
        }
    } catch (const std::exception& exception) {
        finishTask(
            record,
            core::Error{
                core::ErrorCode::InvalidParameter,
                std::string("Task threw an exception: ") + exception.what()});
    } catch (...) {
        finishTask(
            record,
            core::Error{
                core::ErrorCode::InvalidParameter,
                "Task threw an unknown exception."});
    }
}

void Scheduler::reportProgress(
    const std::shared_ptr<TaskRecord>& record,
    int percent,
    std::string message)
{
    const int safePercent = std::clamp(percent, 0, 100);
    TaskSnapshot progressSnapshot;
    bool shouldNotify = false;

    {
        std::lock_guard lock(mutex_);
        if (record->snapshot.status != TaskStatus::Running
            || record->stopSource.stop_requested()
            || safePercent < record->snapshot.progressPercent) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool intervalPassed = !record->hasReportedProgress
            || now - record->lastProgressTime >= config_.progressInterval;
        const bool finalProgress = safePercent == 100
            && record->snapshot.progressPercent < 100;
        if (!intervalPassed && !finalProgress) {
            return;
        }

        record->snapshot.progressPercent = safePercent;
        record->snapshot.progressMessage = std::move(message);
        record->lastProgressTime = now;
        record->hasReportedProgress = true;
        progressSnapshot = record->snapshot;
        shouldNotify = true;
    }

    if (shouldNotify) {
        notifyObserver(record, progressSnapshot);
    }
}

void Scheduler::finishTask(
    const std::shared_ptr<TaskRecord>& record,
    std::optional<core::Error> error)
{
    TaskSnapshot finishedSnapshot;

    {
        std::lock_guard lock(mutex_);
        if (record->snapshot.status != TaskStatus::Running) {
            return;
        }

        const bool cancellationError = error.has_value()
            && error->code == core::ErrorCode::Cancelled;
        // The action result is the commit boundary. A late stop request must
        // not relabel work that already returned success (for example, after
        // a database transaction or file rename was committed).
        if (cancellationError) {
            record->snapshot.status = TaskStatus::Cancelled;
            record->snapshot.errorMessage = error->message;
        } else if (error.has_value()) {
            record->snapshot.status = TaskStatus::Failed;
            record->snapshot.errorMessage = error->message;
        } else {
            record->snapshot.status = TaskStatus::Succeeded;
            record->snapshot.progressPercent = 100;
            record->snapshot.errorMessage.clear();
        }

        finishedSnapshot = record->snapshot;
    }

    stateChanged_.notify_all();
    notifyObserver(record, finishedSnapshot);
}

void Scheduler::cancelDrainedTasks(std::vector<QueuedTask> tasks)
{
    for (const auto& queued : tasks) {
        TaskSnapshot cancelledSnapshot;
        std::shared_ptr<TaskRecord> record;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            const auto item = tasks_.find(queued.id);
            if (item != tasks_.end()
                && item->second->snapshot.status == TaskStatus::Pending) {
                record = item->second;
                record->snapshot.status = TaskStatus::Cancelled;
                record->snapshot.errorMessage = kCancelledMessage;
                cancelledSnapshot = record->snapshot;
                changed = true;
            }
        }

        if (changed) {
            stateChanged_.notify_all();
            notifyObserver(record, cancelledSnapshot);
        }
    }
}

void Scheduler::notifyObserver(
    const std::shared_ptr<TaskRecord>& record,
    const TaskSnapshot& snapshot) noexcept
{
    if (!record || !record->observer) {
        return;
    }

    try {
        bool dispatchNotifications = false;
        {
            // Serialize callbacks for one task by queuing snapshots. Different
            // tasks may still notify concurrently. A terminal notification can
            // race with the initial PENDING callback during shutdown; stale
            // snapshots are discarded before they enter the queue.
            std::lock_guard lock(record->observerMutex);
            const int phase = snapshot.status == TaskStatus::Pending
                ? 0
                : snapshot.status == TaskStatus::Running ? 1 : 2;
            if (phase < record->lastObserverPhase
                || (phase == 1
                    && record->lastObserverPhase == 1
                    && snapshot.progressPercent
                        < record->lastObserverProgress)) {
                return;
            }
            record->lastObserverPhase = phase;
            if (phase == 1) {
                record->lastObserverProgress = snapshot.progressPercent;
            }
            record->observerNotifications.push_back(snapshot);
            if (!record->observerDispatchActive) {
                record->observerDispatchActive = true;
                dispatchNotifications = true;
            }
        }

        if (!dispatchNotifications) {
            return;
        }

        for (;;) {
            TaskSnapshot nextSnapshot;
            {
                std::lock_guard lock(record->observerMutex);
                if (record->observerNotifications.empty()) {
                    record->observerDispatchActive = false;
                    return;
                }
                nextSnapshot = std::move(
                    record->observerNotifications.front());
                record->observerNotifications.pop_front();
            }

            activeObserverCallbacks_.fetch_add(1, std::memory_order_acq_rel);
            try {
                record->observer(nextSnapshot);
            } catch (...) {
                // Observers are notifications only. They cannot break task
                // execution or prevent later queued notifications.
            }
            activeObserverCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
        }
    } catch (...) {
        // Notifications are best effort and cannot break task execution.
    }
}

}  // namespace signallab::task
