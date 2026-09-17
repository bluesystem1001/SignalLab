#include "signallab/task/task_queue.hpp"

#include <algorithm>
#include <utility>

namespace signallab::task {

bool TaskQueue::push(QueuedTask task)
{
    if (!task.run) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    changed_.notify_one();
    return true;
}

std::optional<QueuedTask> TaskQueue::waitPop(std::stop_token stopToken)
{
    std::unique_lock lock(mutex_);
    changed_.wait(lock, stopToken, [this]() {
        return closed_ || !tasks_.empty();
    });

    if (tasks_.empty()) {
        return std::nullopt;
    }

    QueuedTask task = std::move(tasks_.front());
    tasks_.pop_front();
    return task;
}

std::optional<QueuedTask> TaskQueue::remove(TaskId id)
{
    std::lock_guard lock(mutex_);
    const auto item = std::find_if(
        tasks_.begin(),
        tasks_.end(),
        [id](const QueuedTask& task) { return task.id == id; });
    if (item == tasks_.end()) {
        return std::nullopt;
    }

    QueuedTask removed = std::move(*item);
    tasks_.erase(item);
    return removed;
}

std::vector<QueuedTask> TaskQueue::closeAndTakePending()
{
    std::vector<QueuedTask> pending;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        pending.reserve(tasks_.size());
        while (!tasks_.empty()) {
            pending.push_back(std::move(tasks_.front()));
            tasks_.pop_front();
        }
    }
    changed_.notify_all();
    return pending;
}

void TaskQueue::close()
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

bool TaskQueue::isClosed() const
{
    std::lock_guard lock(mutex_);
    return closed_;
}

std::size_t TaskQueue::size() const
{
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

}  // namespace signallab::task
