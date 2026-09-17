#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "signallab/app/import_service.hpp"
#include "signallab/core/result.hpp"
#include "signallab/task/scheduler.hpp"

namespace signallab::app {

enum class BackgroundFileOperation {
    PreviewCsv,
    PreviewWav,
    ImportCsv,
    ImportWav
};

struct BackgroundFileEvent {
    task::TaskSnapshot task;
    BackgroundFileOperation operation{BackgroundFileOperation::PreviewCsv};
    std::filesystem::path sourcePath;
    std::shared_ptr<const repository::ImportPreview> preview;
    // The UI is the sole consumer of a completed entry and can move it into
    // ImportService without copying a potentially large sample buffer.
    std::shared_ptr<SignalEntry> importedSignal;
};

// Routes file work to Scheduler's small I/O pool. Slow work never changes the
// GUI-facing signal list; MainWindow accepts a finished SignalEntry itself.
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
