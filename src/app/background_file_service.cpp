#include "signallab/app/background_file_service.hpp"

#include <string>
#include <unordered_map>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::app {
namespace {

std::string operationName(BackgroundFileOperation operation)
{
    switch (operation) {
    case BackgroundFileOperation::PreviewCsv:
        return "Preview CSV";
    case BackgroundFileOperation::PreviewWav:
        return "Preview WAV";
    case BackgroundFileOperation::ImportCsv:
        return "Import CSV";
    case BackgroundFileOperation::ImportWav:
        return "Import WAV";
    }
    return "File task";
}

}  // namespace

struct BackgroundFileService::JobData {
    BackgroundFileOperation operation{BackgroundFileOperation::PreviewCsv};
    std::filesystem::path sourcePath;
    repository::CsvImportOptions csvOptions;
    repository::WavImportOptions wavOptions;
    std::mutex mutex;
    std::unordered_map<
        task::TaskId,
        std::shared_ptr<const repository::ImportPreview>> previews;
    std::unordered_map<task::TaskId, std::shared_ptr<SignalEntry>>
        importedSignals;
};

struct BackgroundFileService::SharedState {
    std::mutex mutex;
    std::deque<BackgroundFileEvent> events;
    std::function<void()> notifier;
    bool acceptEvents{true};
};

BackgroundFileService::BackgroundFileService(
    ImportService& importService,
    task::Scheduler& scheduler)
    : importService_(importService),
      scheduler_(scheduler),
      state_(std::make_shared<SharedState>())
{
}

BackgroundFileService::~BackgroundFileService()
{
    std::lock_guard lock(state_->mutex);
    state_->acceptEvents = false;
    state_->notifier = {};
    state_->events.clear();
}

core::Result<task::TaskId> BackgroundFileService::previewCsv(
    std::filesystem::path sourcePath,
    repository::CsvImportOptions options)
{
    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::PreviewCsv;
    job->sourcePath = std::move(sourcePath);
    job->csvOptions = std::move(options);
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::previewWav(
    std::filesystem::path sourcePath)
{
    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::PreviewWav;
    job->sourcePath = std::move(sourcePath);
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::importCsv(
    std::filesystem::path sourcePath,
    repository::CsvImportOptions options)
{
    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::ImportCsv;
    job->sourcePath = std::move(sourcePath);
    job->csvOptions = std::move(options);
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::importWav(
    std::filesystem::path sourcePath,
    repository::WavImportOptions options)
{
    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::ImportWav;
    job->sourcePath = std::move(sourcePath);
    job->wavOptions = std::move(options);
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::submitJob(
    std::shared_ptr<JobData> job)
{
    ImportService* importService = &importService_;
    const std::shared_ptr<SharedState> sharedState = state_;

    task::TaskSpec specification;
    specification.name = operationName(job->operation) + ": "
        + job->sourcePath.filename().string();
    specification.lane = task::TaskLane::Io;
    specification.action = [importService, job](
                               const task::TaskContext& context) {
        {
            std::lock_guard lock(job->mutex);
            job->previews.erase(context.id());
            job->importedSignals.erase(context.id());
        }

        context.reportProgress(0, "Reading the file");

        if (job->operation == BackgroundFileOperation::PreviewCsv) {
            auto result = importService->previewCsv(
                job->sourcePath,
                job->csvOptions,
                context.stopToken());
            if (!result) {
                return core::Result<bool>::failure(result.error());
            }
            std::lock_guard lock(job->mutex);
            job->previews[context.id()] =
                std::make_shared<repository::ImportPreview>(
                std::move(result).value());
        } else if (job->operation == BackgroundFileOperation::PreviewWav) {
            auto result = importService->previewWav(
                job->sourcePath,
                context.stopToken());
            if (!result) {
                return core::Result<bool>::failure(result.error());
            }
            std::lock_guard lock(job->mutex);
            job->previews[context.id()] =
                std::make_shared<repository::ImportPreview>(
                std::move(result).value());
        } else if (job->operation == BackgroundFileOperation::ImportCsv) {
            auto result = importService->loadCsv(
                job->sourcePath,
                job->csvOptions,
                context.stopToken());
            if (!result) {
                return core::Result<bool>::failure(result.error());
            }
            std::lock_guard lock(job->mutex);
            job->importedSignals[context.id()] =
                std::make_shared<SignalEntry>(
                std::move(result).value());
        } else if (job->operation == BackgroundFileOperation::ImportWav) {
            auto result = importService->loadWav(
                job->sourcePath,
                job->wavOptions,
                context.stopToken());
            if (!result) {
                return core::Result<bool>::failure(result.error());
            }
            std::lock_guard lock(job->mutex);
            job->importedSignals[context.id()] =
                std::make_shared<SignalEntry>(
                std::move(result).value());
        } else {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Unknown background file operation.");
        }

        context.reportProgress(100, "File task complete");
        return core::Result<bool>::success(true);
    };
    specification.observer = [sharedState, job](
                                 const task::TaskSnapshot& snapshot) {
        std::shared_ptr<const repository::ImportPreview> preview;
        std::shared_ptr<SignalEntry> importedSignal;
        if (task::isTerminal(snapshot.status)) {
            std::lock_guard lock(job->mutex);
            const auto previewItem = job->previews.find(snapshot.id);
            if (previewItem != job->previews.end()) {
                preview = std::move(previewItem->second);
                job->previews.erase(previewItem);
            }
            const auto signalItem = job->importedSignals.find(snapshot.id);
            if (signalItem != job->importedSignals.end()) {
                importedSignal = std::move(signalItem->second);
                job->importedSignals.erase(signalItem);
            }
        }

        std::function<void()> notifier;
        {
            std::lock_guard lock(sharedState->mutex);
            if (!sharedState->acceptEvents) {
                return;
            }
            sharedState->events.push_back(BackgroundFileEvent{
                snapshot,
                job->operation,
                job->sourcePath,
                std::move(preview),
                std::move(importedSignal)});
            notifier = sharedState->notifier;
        }

        if (notifier) {
            notifier();
        }
    };

    return scheduler_.submit(std::move(specification));
}

void BackgroundFileService::setEventNotifier(
    std::function<void()> notifier)
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = std::move(notifier);
}

void BackgroundFileService::clearEventNotifier()
{
    std::lock_guard lock(state_->mutex);
    state_->notifier = {};
}

std::vector<BackgroundFileEvent> BackgroundFileService::takeEvents()
{
    std::vector<BackgroundFileEvent> events;
    std::lock_guard lock(state_->mutex);
    events.reserve(state_->events.size());
    while (!state_->events.empty()) {
        events.push_back(std::move(state_->events.front()));
        state_->events.pop_front();
    }
    return events;
}

}  // namespace signallab::app
