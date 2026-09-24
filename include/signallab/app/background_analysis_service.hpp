#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "signallab/app/analysis_service.hpp"
#include "signallab/core/result.hpp"
#include "signallab/task/scheduler.hpp"

namespace signallab::app {

struct BackgroundAnalysisEvent {
    task::TaskSnapshot task;
    std::int64_t signalRecordId{0};
    AnalysisOptions options;
    std::shared_ptr<const AnalysisResult> result;
};

// Adapts AnalysisService to Scheduler without depending on Qt. The notifier
// may run on a worker thread; MainWindow forwards it with a queued Qt call.
class BackgroundAnalysisService {
public:
    BackgroundAnalysisService(
        AnalysisService& analysisService,
        task::Scheduler& scheduler);
    ~BackgroundAnalysisService();

    BackgroundAnalysisService(const BackgroundAnalysisService&) = delete;
    BackgroundAnalysisService& operator=(
        const BackgroundAnalysisService&) = delete;

    [[nodiscard]] core::Result<task::TaskId> submit(
        std::int64_t signalRecordId,
        AnalysisOptions options = {});

    void setEventNotifier(std::function<void()> notifier);
    void clearEventNotifier();

    [[nodiscard]] std::vector<BackgroundAnalysisEvent> takeEvents();

private:
    struct SharedState;

    AnalysisService& analysisService_;
    task::Scheduler& scheduler_;
    std::shared_ptr<SharedState> state_;
};

}  // namespace signallab::app
