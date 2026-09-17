#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include "signallab/task/task.hpp"

namespace signallab::task {

struct QueuedTask {
    TaskId id{0};
    std::function<void()> run;
};

// A small blocking FIFO queue. close() wakes waiting workers. Existing items
// can still be consumed; closeAndTakePending() closes and removes them instead.
class TaskQueue {
public:
    [[nodiscard]] bool push(QueuedTask task);

    [[nodiscard]] std::optional<QueuedTask> waitPop(
        std::stop_token stopToken = {});

    [[nodiscard]] std::optional<QueuedTask> remove(TaskId id);
    [[nodiscard]] std::vector<QueuedTask> closeAndTakePending();

    void close();

    [[nodiscard]] bool isClosed() const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<QueuedTask> tasks_;
    bool closed_{false};
};

}  // namespace signallab::task
