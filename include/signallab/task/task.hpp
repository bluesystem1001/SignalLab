#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>

#include "signallab/core/result.hpp"

namespace signallab::task {

using TaskId = std::uint64_t;

enum class TaskStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled
};

enum class TaskLane {
    Compute,
    Io
};

struct TaskSnapshot {
    TaskId id{0};
    std::string name;
    TaskLane lane{TaskLane::Compute};
    TaskStatus status{TaskStatus::Pending};
    int progressPercent{0};
    std::string progressMessage;
    std::string errorMessage;
    TaskId retryOf{0};
};

using TaskObserver = std::function<void(const TaskSnapshot&)>;

// TaskContext is passed to a task while it is running. Long loops should
// check stopRequested() regularly and return ErrorCode::Cancelled when true.
class TaskContext {
public:
    [[nodiscard]] TaskId id() const noexcept;
    [[nodiscard]] std::stop_token stopToken() const noexcept;
    [[nodiscard]] bool stopRequested() const noexcept;

    void reportProgress(int percent, std::string message = {}) const;

private:
    using ProgressFunction = std::function<void(int, std::string)>;

    friend class Scheduler;

    TaskContext(
        TaskId id,
        std::stop_token stopToken,
        ProgressFunction progressFunction);

    TaskId id_;
    std::stop_token stopToken_;
    ProgressFunction progressFunction_;
};

// A successful Result is the task's commit boundary and produces SUCCEEDED,
// even if a cancellation request arrives at the same time. Cooperative work
// returns ErrorCode::Cancelled before committing to produce CANCELLED. The
// bool value is kept only because Result has no void specialization.
using TaskAction = std::function<core::Result<bool>(const TaskContext&)>;

struct TaskSpec {
    std::string name;
    TaskLane lane{TaskLane::Compute};
    TaskAction action;
    TaskObserver observer;
};

[[nodiscard]] bool isTerminal(TaskStatus status) noexcept;

}  // namespace signallab::task
