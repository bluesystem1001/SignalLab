#include "signallab/app/background_file_service.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "signallab/algorithm/fir_filter.hpp"
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
    case BackgroundFileOperation::FilterFir:
        return "FIR filter";
    case BackgroundFileOperation::DeleteSignal:
        return "Delete signal";
    }
    return "File task";
}

std::string_view firTypeName(FirFilterType type)
{
    switch (type) {
    case FirFilterType::LowPass:
        return "lowpass";
    case FirFilterType::HighPass:
        return "highpass";
    }
    return "unknown";
}

std::string_view firTypeDisplayName(FirFilterType type)
{
    switch (type) {
    case FirFilterType::LowPass:
        return "low-pass";
    case FirFilterType::HighPass:
        return "high-pass";
    }
    return "unknown";
}

std::string canonicalNumber(double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(
        std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

bool isLowercaseSha256(std::string_view value)
{
    if (value.size() != 64U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string firSourceKey(
    const SignalEntry& source,
    const FirFilterOptions& options)
{
    return "generated:fir:version="
        + std::string(algorithm::kFirFilterAlgorithmVersion)
        + "|input-record=" + std::to_string(source.recordId)
        + "|input-sha256=" + source.sampleHash
        + "|type=" + std::string(firTypeName(options.type))
        + "|cutoff-hz=" + canonicalNumber(options.cutoffHz)
        + "|tap-count=" + std::to_string(options.tapCount)
        + "|boundary=zero-padding";
}

std::string firParametersJson(
    const SignalEntry& source,
    const FirFilterOptions& options)
{
    return "{\"algorithm\":\"fir\",\"algorithm_version\":\""
        + std::string(algorithm::kFirFilterAlgorithmVersion)
        + "\",\"boundary\":\"zero_padding\",\"cutoff_hz\":"
        + canonicalNumber(options.cutoffHz)
        + ",\"input_record_id\":" + std::to_string(source.recordId)
        + ",\"input_sample_hash\":\"" + source.sampleHash
        + "\",\"tap_count\":" + std::to_string(options.tapCount)
        + ",\"type\":\"" + std::string(firTypeName(options.type))
        + "\"}";
}

core::SignalMetadata firOutputMetadata(
    const SignalEntry& source,
    const FirFilterOptions& options)
{
    core::SignalMetadata metadata = source.signal.metadata();
    std::string inputName = source.displayName;
    if (inputName.empty()) {
        inputName = metadata.name;
    }
    if (inputName.empty()) {
        inputName = "Signal #" + std::to_string(source.recordId);
    }

    metadata.name = inputName + " [FIR "
        + std::string(firTypeDisplayName(options.type)) + " "
        + canonicalNumber(options.cutoffHz) + " Hz, "
        + std::to_string(options.tapCount) + " taps]";
    const std::string originalSource = metadata.source;
    metadata.source = "FIR-derived from signal #"
        + std::to_string(source.recordId);
    if (!originalSource.empty()) {
        metadata.source += "; " + originalSource;
    }
    return metadata;
}

}  // namespace

core::Result<bool> validateFirFilterOptions(
    double sampleRate,
    const FirFilterOptions& options)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidSampleRate,
            "FIR filtering requires a finite positive sample rate.");
    }
    if (options.type != FirFilterType::LowPass
        && options.type != FirFilterType::HighPass) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The FIR filter type is invalid.");
    }
    if (options.tapCount < 3U || options.tapCount % 2U == 0U
        || options.tapCount > kMaximumFirTapCount) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The FIR tap count must be an odd integer from 3 through "
                + std::to_string(kMaximumFirTapCount) + ".");
    }
    if (!std::isfinite(options.cutoffHz) || options.cutoffHz <= 0.0
        || options.cutoffHz >= sampleRate / 2.0) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The FIR cutoff must be finite and strictly between zero and "
            "the Nyquist frequency.");
    }
    return core::Result<bool>::success(true);
}

struct BackgroundFileService::JobData {
    BackgroundFileOperation operation{BackgroundFileOperation::PreviewCsv};
    std::filesystem::path sourcePath;
    repository::CsvImportOptions csvOptions;
    repository::WavImportOptions wavOptions;
    std::optional<SignalEntry> sourceSignal;
    std::int64_t signalRecordId{0};
    std::string signalDisplayName;
    FirFilterOptions firOptions;
    std::mutex mutex;
    std::unordered_map<
        task::TaskId,
        std::shared_ptr<const repository::ImportPreview>> previews;
    std::unordered_map<task::TaskId, std::shared_ptr<SignalEntry>>
        importedSignals;
    std::unordered_map<task::TaskId, std::shared_ptr<SignalRemovalOutcome>>
        signalRemovals;
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

core::Result<task::TaskId> BackgroundFileService::filterFir(
    const SignalEntry& sourceSignal,
    FirFilterOptions options)
{
    if (sourceSignal.recordId <= 0
        || !isLowercaseSha256(sourceSignal.sampleHash)) {
        return core::Result<task::TaskId>::failure(
            core::ErrorCode::InvalidParameter,
            "FIR filtering requires a persisted source signal with a valid "
            "sample hash.");
    }
    auto validation = validateFirFilterOptions(
        sourceSignal.signal.sampleRate(), options);
    if (!validation) {
        return core::Result<task::TaskId>::failure(validation.error());
    }

    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::FilterFir;
    job->sourceSignal = sourceSignal;
    job->firOptions = options;
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::deleteSignal(
    std::int64_t recordId,
    std::string displayName)
{
    if (recordId <= 0) {
        return core::Result<task::TaskId>::failure(
            core::ErrorCode::InvalidParameter,
            "Signal deletion requires a positive record id.");
    }

    auto job = std::make_shared<JobData>();
    job->operation = BackgroundFileOperation::DeleteSignal;
    job->signalRecordId = recordId;
    job->signalDisplayName = std::move(displayName);
    return submitJob(std::move(job));
}

core::Result<task::TaskId> BackgroundFileService::submitJob(
    std::shared_ptr<JobData> job)
{
    ImportService* importService = &importService_;
    const std::shared_ptr<SharedState> sharedState = state_;

    task::TaskSpec specification;
    if (job->operation == BackgroundFileOperation::FilterFir
        && job->sourceSignal.has_value()) {
        specification.name = operationName(job->operation) + ": "
            + job->sourceSignal->displayName;
    } else if (job->operation
               == BackgroundFileOperation::DeleteSignal) {
        specification.name = operationName(job->operation) + ": "
            + (job->signalDisplayName.empty()
                   ? "signal #" + std::to_string(job->signalRecordId)
                   : job->signalDisplayName);
    } else {
        specification.name = operationName(job->operation) + ": "
            + job->sourcePath.filename().string();
    }
    specification.lane =
        job->operation == BackgroundFileOperation::FilterFir
        ? task::TaskLane::Compute
        : task::TaskLane::Io;
    specification.action = [importService, job](
                               const task::TaskContext& context) {
        {
            std::lock_guard lock(job->mutex);
            job->previews.erase(context.id());
            job->importedSignals.erase(context.id());
            job->signalRemovals.erase(context.id());
        }

        context.reportProgress(
            0,
            job->operation == BackgroundFileOperation::FilterFir
                ? "Preparing FIR filtering"
                : job->operation == BackgroundFileOperation::DeleteSignal
                    ? "Deleting the persisted signal"
                    : "Reading the file");

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
        } else if (job->operation
                   == BackgroundFileOperation::FilterFir) {
            if (!job->sourceSignal.has_value()) {
                return core::Result<bool>::failure(
                    core::ErrorCode::InvalidParameter,
                    "The FIR task has no source signal snapshot.");
            }
            const SignalEntry& source = *job->sourceSignal;
            context.reportProgress(5, "Designing FIR coefficients");
            auto coefficients = job->firOptions.type
                    == FirFilterType::LowPass
                ? algorithm::designLowPassFir(
                      job->firOptions.tapCount,
                      job->firOptions.cutoffHz,
                      source.signal.sampleRate())
                : algorithm::designHighPassFir(
                      job->firOptions.tapCount,
                      job->firOptions.cutoffHz,
                      source.signal.sampleRate());
            if (!coefficients) {
                return core::Result<bool>::failure(
                    coefficients.error());
            }

            context.reportProgress(15, "Filtering samples");
            auto filtered = algorithm::applyFir(
                source.signal,
                coefficients.value(),
                algorithm::FirBoundaryStrategy::ZeroPadding,
                context.stopToken());
            if (!filtered) {
                return core::Result<bool>::failure(filtered.error());
            }
            core::Signal output = std::move(filtered).value().withMetadata(
                firOutputMetadata(source, job->firOptions));

            context.reportProgress(90, "Saving filtered signal");
            auto persisted = importService->persistGeneratedSignal(
                std::move(output),
                firSourceKey(source, job->firOptions),
                firParametersJson(source, job->firOptions),
                context.stopToken());
            if (!persisted) {
                return core::Result<bool>::failure(persisted.error());
            }
            std::lock_guard lock(job->mutex);
            job->importedSignals[context.id()] =
                std::make_shared<SignalEntry>(
                    std::move(persisted).value());
        } else if (job->operation
                   == BackgroundFileOperation::DeleteSignal) {
            auto removed = importService->removePersistedSignal(
                job->signalRecordId);
            if (!removed) {
                return core::Result<bool>::failure(removed.error());
            }
            std::lock_guard lock(job->mutex);
            job->signalRemovals[context.id()] =
                std::make_shared<SignalRemovalOutcome>(
                    std::move(removed).value());
        } else {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Unknown background file operation.");
        }

        context.reportProgress(
            100,
            job->operation == BackgroundFileOperation::FilterFir
                ? "FIR filtering complete"
                : job->operation == BackgroundFileOperation::DeleteSignal
                    ? "Signal deletion complete"
                    : "File task complete");
        return core::Result<bool>::success(true);
    };
    specification.observer = [sharedState, job](
                                 const task::TaskSnapshot& snapshot) {
        std::shared_ptr<const repository::ImportPreview> preview;
        std::shared_ptr<SignalEntry> importedSignal;
        std::shared_ptr<SignalRemovalOutcome> signalRemoval;
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
            const auto removalItem = job->signalRemovals.find(snapshot.id);
            if (removalItem != job->signalRemovals.end()) {
                signalRemoval = std::move(removalItem->second);
                job->signalRemovals.erase(removalItem);
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
                std::move(importedSignal),
                std::move(signalRemoval),
                job->sourceSignal.has_value()
                    ? job->sourceSignal->recordId
                    : job->signalRecordId,
                job->operation == BackgroundFileOperation::FilterFir
                    ? std::optional<FirFilterOptions>(job->firOptions)
                    : std::nullopt});
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
