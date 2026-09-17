#include "signallab/app/background_analysis_service.hpp"

#include <string>
#include <unordered_map>
#include <utility>

namespace signallab::app {
namespace {

struct AnalysisJobData {
    std::int64_t signalRecordId{0};
    AnalysisOptions options;
    std::mutex mutex;
    std::unordered_map<task::TaskId, std::shared_ptr<const AnalysisResult>>
        results;
};

}  // namespace

struct BackgroundAnalysisService::SharedState {
    std::mutex mutex;
    std::deque<BackgroundAnalysisEvent> events;
    std::function<void()> notifier;
    bool acceptEvents{true};
};

BackgroundAnalysisService::BackgroundAnalysisService(
    AnalysisService& analysisService,
    task::Scheduler& scheduler)
    : analysisService_(analysisService),
      scheduler_(scheduler),
      state_(std::make_shared<SharedState>())
{
}

BackgroundAnalysisService::~BackgroundAnalysisService()
{
    std::lock_guard lock(state_->mutex);
    state_->acceptEvents = false;
    state_->notifier = {};
    state_->events.clear();
}

core::Result<task::TaskId> BackgroundAnalysisService::submit(
    std::int64_t signalRecordId,
    AnalysisOptions options)
{
    auto job = std::make_shared<AnalysisJobData>();
    job->signalRecordId = signalRecordId;
    job->options = std::move(options);

    AnalysisService* analysisService = &analysisService_;
    const std::shared_ptr<SharedState> sharedState = state_;

    task::TaskSpec specification;
    specification.name = "Analyze signal #"
        + std::to_string(signalRecordId);
    specification.lane = task::TaskLane::Compute;
    specification.action = [analysisService, job](
                               const task::TaskContext& context) {
        {
            std::lock_guard lock(job->mutex);
            job->results.erase(context.id());
        }

        auto analysisResult = analysisService->analyze(
            job->signalRecordId,
            job->options,
            context.stopToken(),
            [&context](int percent, std::string message) {
                context.reportProgress(percent, std::move(message));
            });
        if (!analysisResult) {
            return core::Result<bool>::failure(analysisResult.error());
        }

        {
            std::lock_guard lock(job->mutex);
            job->results[context.id()] = std::make_shared<AnalysisResult>(
                std::move(analysisResult).value());
        }
        return core::Result<bool>::success(true);
    };
    specification.observer = [sharedState, job](
                                 const task::TaskSnapshot& snapshot) {
        std::shared_ptr<const AnalysisResult> result;
        if (task::isTerminal(snapshot.status)) {
            std::lock_guard lock(job->mutex);
            const auto item = job->results.find(snapshot.id);
            if (item != job->results.end()) {
                result = std::move(item->second);
                job->results.erase(item);
            }
        }

        std::function<void()> notifier;
        {
            std::lock_guard lock(sharedState->mutex);
            if (!sharedState->acceptEvents) {
                return;
            }
            sharedState->events.push_back(BackgroundAnalysisEvent{
                snapshot,
                job->signalRecordId,
                std::move(result)});
            notifier = sharedState->notifier;
        }

        if (notifier) {
            notifier();
        }
    };

    return scheduler_.submit(std::move(specification));
}

void BackgroundAnalysisService::setEventNotifier(
    std::function<void()> notifier)
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = std::move(notifier);
}

void BackgroundAnalysisService::clearEventNotifier()
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = {};
}

std::vector<BackgroundAnalysisEvent>
BackgroundAnalysisService::takeEvents()
{
    std::vector<BackgroundAnalysisEvent> events;
    std::lock_guard lock(state_->mutex);
    events.reserve(state_->events.size());
    while (!state_->events.empty()) {
        events.push_back(std::move(state_->events.front()));
        state_->events.pop_front();
    }
    return events;
}

}  // namespace signallab::app
