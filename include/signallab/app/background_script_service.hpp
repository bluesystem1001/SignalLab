#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "signallab/app/script_service.hpp"
#include "signallab/core/result.hpp"
#include "signallab/task/scheduler.hpp"

namespace signallab::app {

enum class BackgroundScriptOperation {
    Execute,
    History
};

struct BackgroundScriptEvent {
    task::TaskSnapshot task;
    BackgroundScriptOperation operation{BackgroundScriptOperation::Execute};
    std::shared_ptr<const std::string> source;
    std::shared_ptr<ScriptExecutionResult> result;
    std::shared_ptr<
        std::vector<repository::ScriptHistoryRecord>> history;
};

// Runs execution on the compute pool and history reads on the I/O pool.
// SignalScript may perform FFT/FIR work, so the GUI must only consume the
// queued event stream. A terminal execution event owns a mutable result so
// that the GUI can move large opened signals into its model without copying.
class BackgroundScriptService {
public:
    BackgroundScriptService(
        ScriptService& scriptService,
        task::Scheduler& scheduler);
    ~BackgroundScriptService();

    BackgroundScriptService(const BackgroundScriptService&) = delete;
    BackgroundScriptService& operator=(
        const BackgroundScriptService&) = delete;

    [[nodiscard]] core::Result<task::TaskId> submit(std::string source);
    [[nodiscard]] core::Result<task::TaskId> requestHistory(
        std::size_t maximumCount = 100);

    void setEventNotifier(std::function<void()> notifier);
    void clearEventNotifier();

    [[nodiscard]] std::vector<BackgroundScriptEvent> takeEvents();

private:
    struct SharedState;

    ScriptService& scriptService_;
    task::Scheduler& scheduler_;
    std::shared_ptr<SharedState> state_;
};

}  // namespace signallab::app
