#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "signallab/app/import_service.hpp"
#include "signallab/core/result.hpp"
#include "signallab/task/scheduler.hpp"

namespace signallab::app {

enum class FirFilterType {
    LowPass,
    HighPass
};

inline constexpr std::size_t kMaximumFirTapCount = 65'535U;

struct FirFilterOptions {
    FirFilterType type{FirFilterType::LowPass};
    std::size_t tapCount{63U};
    double cutoffHz{100.0};
};

// Shared by the GUI's immediate validation and the background submission
// boundary. The algorithm performs the same defensive checks again.
[[nodiscard]] core::Result<bool> validateFirFilterOptions(
    double sampleRate,
    const FirFilterOptions& options);

enum class BackgroundFileOperation {
    PreviewCsv,
    PreviewWav,
    ImportCsv,
    ImportWav,
    FilterFir,
    DeleteSignal
};

struct BackgroundFileEvent {
    task::TaskSnapshot task;
    BackgroundFileOperation operation{BackgroundFileOperation::PreviewCsv};
    std::filesystem::path sourcePath;
    std::shared_ptr<const repository::ImportPreview> preview;
    // The UI is the sole consumer of a completed entry and can move it into
    // ImportService without copying a potentially large sample buffer.
    std::shared_ptr<SignalEntry> importedSignal;
    std::shared_ptr<SignalRemovalOutcome> signalRemoval;
    std::int64_t sourceRecordId{0};
    std::optional<FirFilterOptions> firOptions;
};

// Routes file work to Scheduler's small I/O pool and FIR work to its compute
// pool. Slow work never changes the GUI-facing signal list; MainWindow accepts
// a finished SignalEntry itself.
class BackgroundFileService {
public:
    BackgroundFileService(
        ImportService& importService,
        task::Scheduler& scheduler);
    ~BackgroundFileService();

    BackgroundFileService(const BackgroundFileService&) = delete;
    BackgroundFileService& operator=(const BackgroundFileService&) = delete;

    [[nodiscard]] core::Result<task::TaskId> previewCsv(
        std::filesystem::path sourcePath,
        repository::CsvImportOptions options);

    [[nodiscard]] core::Result<task::TaskId> previewWav(
        std::filesystem::path sourcePath);

    [[nodiscard]] core::Result<task::TaskId> importCsv(
        std::filesystem::path sourcePath,
        repository::CsvImportOptions options);

    [[nodiscard]] core::Result<task::TaskId> importWav(
        std::filesystem::path sourcePath,
        repository::WavImportOptions options);

    // The input entry is snapshotted at submission time. Signal copies share
    // their immutable sample buffer, so selection changes and vector growth
    // in the GUI cannot invalidate a running filter task.
    [[nodiscard]] core::Result<task::TaskId> filterFir(
        const SignalEntry& sourceSignal,
        FirFilterOptions options);

    [[nodiscard]] core::Result<task::TaskId> deleteSignal(
        std::int64_t recordId,
        std::string displayName);

    void setEventNotifier(std::function<void()> notifier);
    void clearEventNotifier();

    [[nodiscard]] std::vector<BackgroundFileEvent> takeEvents();

private:
    struct JobData;
    struct SharedState;

    [[nodiscard]] core::Result<task::TaskId> submitJob(
        std::shared_ptr<JobData> job);

    ImportService& importService_;
    task::Scheduler& scheduler_;
    std::shared_ptr<SharedState> state_;
};

}  // namespace signallab::app
