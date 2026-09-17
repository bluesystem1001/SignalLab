#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "signallab/task/task_queue.hpp"

namespace signallab::task {

// A fixed-size pool. Task exceptions are contained by the worker loop so one
// bad low-level work item cannot terminate the process.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t workerCount);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    [[nodiscard]] bool submit(QueuedTask task);
    [[nodiscard]] std::optional<QueuedTask> removePending(TaskId id);
    [[nodiscard]] std::vector<QueuedTask> stopAccepting();

    void requestStop();
    void join();
    void shutdown();

    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] bool isCurrentThreadWorker() const noexcept;

private:
    void workerLoop(std::stop_token stopToken);

    TaskQueue queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdownMutex_;
    bool joined_{false};
};

}  // namespace signallab::task
