#include "signallab/task/thread_pool.hpp"

#include <stdexcept>
#include <utility>

namespace signallab::task {
namespace {

thread_local const ThreadPool* currentWorkerPool = nullptr;

class WorkerPoolScope {
public:
    explicit WorkerPoolScope(const ThreadPool* pool) noexcept
        : previous_(currentWorkerPool)
    {
        currentWorkerPool = pool;
    }

    ~WorkerPoolScope()
    {
        currentWorkerPool = previous_;
    }

private:
    const ThreadPool* previous_;
};

}  // namespace

ThreadPool::ThreadPool(std::size_t workerCount)
{
    if (workerCount == 0) {
        throw std::invalid_argument(
            "A thread pool must contain at least one worker.");
    }

    workers_.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers_.emplace_back(
            [this](std::stop_token stopToken) { workerLoop(stopToken); });
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

bool ThreadPool::submit(QueuedTask task)
{
    return queue_.push(std::move(task));
}

std::optional<QueuedTask> ThreadPool::removePending(TaskId id)
{
    return queue_.remove(id);
}

std::vector<QueuedTask> ThreadPool::stopAccepting()
{
    return queue_.closeAndTakePending();
}

void ThreadPool::requestStop()
{
    queue_.close();
    for (auto& worker : workers_) {
        worker.request_stop();
    }
}

void ThreadPool::join()
{
    if (isCurrentThreadWorker()) {
        // The owning thread (or destructor) performs the eventual join after
        // this worker returns from its task and leaves workerLoop().
        return;
    }

    std::lock_guard lock(shutdownMutex_);
    if (joined_) {
        return;
    }

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    joined_ = true;
}

void ThreadPool::shutdown()
{
    static_cast<void>(queue_.closeAndTakePending());
    requestStop();
    join();
}

std::size_t ThreadPool::workerCount() const noexcept
{
    return workers_.size();
}

bool ThreadPool::isCurrentThreadWorker() const noexcept
{
    return currentWorkerPool == this;
}

void ThreadPool::workerLoop(std::stop_token stopToken)
{
    const WorkerPoolScope workerScope(this);
    while (true) {
        auto task = queue_.waitPop(stopToken);
        if (!task.has_value()) {
            return;
        }

        try {
            task->run();
        } catch (...) {
            // Scheduler wrappers already convert user exceptions into FAILED.
            // This final boundary keeps a raw ThreadPool item from terminating
            // the process.
        }
    }
}

}  // namespace signallab::task
