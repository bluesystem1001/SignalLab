#include "signallab/app/background_script_service.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::app {
namespace {

struct ScriptJobData {
    std::shared_ptr<const std::string> source;
    std::mutex mutex;
    std::unordered_map<
        task::TaskId,
        std::shared_ptr<ScriptExecutionResult>> results;
};

struct ScriptHistoryJobData {
    std::size_t maximumCount{100};
    std::mutex mutex;
    std::unordered_map<
        task::TaskId,
        std::shared_ptr<
            std::vector<repository::ScriptHistoryRecord>>> results;
};

std::string executionFailureMessage(const ScriptExecutionResult& result)
{
    if (result.runtimeIssue.has_value()) {
        return result.runtimeIssue->code + ": "
            + result.runtimeIssue->message;
    }
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == parser::DiagnosticSeverity::Error) {
            return diagnostic.code + ": " + diagnostic.message;
        }
    }
    return "SignalScript execution failed.";
}

}  // namespace

struct BackgroundScriptService::SharedState {
    std::mutex mutex;
    std::deque<BackgroundScriptEvent> events;
    std::function<void()> notifier;
    bool acceptEvents{true};
};

BackgroundScriptService::BackgroundScriptService(
    ScriptService& scriptService,
    task::Scheduler& scheduler)
    : scriptService_(scriptService),
      scheduler_(scheduler),
      state_(std::make_shared<SharedState>())
{
}

BackgroundScriptService::~BackgroundScriptService()
{
    std::lock_guard lock(state_->mutex);
    state_->acceptEvents = false;
    state_->notifier = {};
    state_->events.clear();
}

core::Result<task::TaskId> BackgroundScriptService::submit(
    std::string source)
{
    auto job = std::make_shared<ScriptJobData>();
    job->source = std::make_shared<const std::string>(std::move(source));

    ScriptService* scriptService = &scriptService_;
    const std::shared_ptr<SharedState> sharedState = state_;

    task::TaskSpec specification;
    specification.name = "Run SignalScript";
    specification.lane = task::TaskLane::Compute;
    specification.action = [scriptService, job](
                               const task::TaskContext& context) {
        {
            std::lock_guard lock(job->mutex);
            job->results.erase(context.id());
        }

        context.reportProgress(5, "Parsing and executing SignalScript");
        auto execution = scriptService->execute(
            *job->source,
            context.stopToken());
        if (!execution) {
            return core::Result<bool>::failure(execution.error());
        }

        auto result = std::make_shared<ScriptExecutionResult>(
            std::move(execution).value());
        {
            std::lock_guard lock(job->mutex);
            job->results[context.id()] = result;
        }

        switch (result->status) {
        case ScriptExecutionStatus::Succeeded:
            context.reportProgress(100, "SignalScript completed");
            return core::Result<bool>::success(true);
        case ScriptExecutionStatus::Cancelled:
            return core::Result<bool>::failure(
                core::ErrorCode::Cancelled,
                executionFailureMessage(*result));
        case ScriptExecutionStatus::Failed:
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                executionFailureMessage(*result));
        }

        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "SignalScript returned an unknown status.");
    };
    specification.observer = [sharedState, job](
                                 const task::TaskSnapshot& snapshot) {
        std::shared_ptr<ScriptExecutionResult> result;
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
            sharedState->events.push_back(BackgroundScriptEvent{
                snapshot,
                BackgroundScriptOperation::Execute,
                job->source,
                std::move(result),
                {}});
            notifier = sharedState->notifier;
        }
        if (notifier) {
            notifier();
        }
    };

    return scheduler_.submit(std::move(specification));
}

core::Result<task::TaskId> BackgroundScriptService::requestHistory(
    std::size_t maximumCount)
{
    auto job = std::make_shared<ScriptHistoryJobData>();
    job->maximumCount = maximumCount;

    ScriptService* scriptService = &scriptService_;
    const std::shared_ptr<SharedState> sharedState = state_;

    task::TaskSpec specification;
    specification.name = "Load SignalScript history";
    specification.lane = task::TaskLane::Io;
    specification.action = [scriptService, job](
                               const task::TaskContext& context) {
        {
            std::lock_guard lock(job->mutex);
            job->results.erase(context.id());
        }

        if (context.stopRequested()) {
            return core::Result<bool>::failure(
                core::ErrorCode::Cancelled,
                "SignalScript history request cancelled.");
        }

        context.reportProgress(10, "Loading SignalScript history");
        auto loaded = scriptService->history(job->maximumCount);
        if (!loaded) {
            return core::Result<bool>::failure(loaded.error());
        }

        auto history = std::make_shared<
            std::vector<repository::ScriptHistoryRecord>>(
            std::move(loaded).value());
        {
            std::lock_guard lock(job->mutex);
            job->results[context.id()] = std::move(history);
        }
        context.reportProgress(100, "SignalScript history loaded");
        return core::Result<bool>::success(true);
    };
    specification.observer = [sharedState, job](
                                 const task::TaskSnapshot& snapshot) {
        std::shared_ptr<
            std::vector<repository::ScriptHistoryRecord>> history;
        if (task::isTerminal(snapshot.status)) {
            std::lock_guard lock(job->mutex);
            const auto item = job->results.find(snapshot.id);
            if (item != job->results.end()) {
                history = std::move(item->second);
                job->results.erase(item);
            }
        }

        std::function<void()> notifier;
        {
            std::lock_guard lock(sharedState->mutex);
            if (!sharedState->acceptEvents) {
                return;
            }
            sharedState->events.push_back(BackgroundScriptEvent{
                snapshot,
                BackgroundScriptOperation::History,
                {},
                {},
                std::move(history)});
            notifier = sharedState->notifier;
        }
        if (notifier) {
            notifier();
        }
    };

    return scheduler_.submit(std::move(specification));
}

void BackgroundScriptService::setEventNotifier(
    std::function<void()> notifier)
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = std::move(notifier);
}

void BackgroundScriptService::clearEventNotifier()
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = {};
}

std::vector<BackgroundScriptEvent> BackgroundScriptService::takeEvents()
{
    std::vector<BackgroundScriptEvent> events;
    std::lock_guard lock(state_->mutex);
    events.reserve(state_->events.size());
    while (!state_->events.empty()) {
        events.push_back(std::move(state_->events.front()));
        state_->events.pop_front();
    }
    return events;
}

}  // namespace signallab::app
