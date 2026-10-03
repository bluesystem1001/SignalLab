#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <vector>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/repository/file_repository.hpp"
#include "signallab/repository/signal_repository.hpp"

namespace signallab::app {

struct SignalEntry {
    std::int64_t recordId;
    std::int64_t sourceId;
    repository::SignalSourceType sourceType;
    std::string sourceKey;
    std::string displayName;
    // The file content hash is empty for generated signals.
    std::string hash;
    std::string sampleHash;
    std::filesystem::path sourcePath;
    std::filesystem::path storedRelativePath;
    std::string parametersJson;
    core::Signal signal;
};

struct SignalRemovalOutcome {
    bool removed{false};
    std::filesystem::path unreferencedStoredRelativePath;
    bool storedFileRemoved{false};
    bool storedFileAlreadyMissing{false};
    // A database commit is authoritative. A physical archive cleanup failure
    // is reported here without relabelling the logical deletion as failed.
    std::string cleanupWarning;
};

class ImportService {
public:
    ImportService(
        repository::FileRepository& fileRepository,
        repository::SignalRepository& signalRepository);

    // Reloads all persisted signal records. The in-memory list changes only
    // after every record and stored file has been checked successfully.
    [[nodiscard]] core::Result<std::size_t> restoreSignals();

    // A stable source key makes generated signals idempotent across restarts.
    [[nodiscard]] core::Result<std::size_t> addGeneratedSignal(
        core::Signal signal,
        const std::string& stableSourceKey);

    // Performs the slow generated-signal lookup/save without changing
    // entries(). Background compute workers use this and hand the returned
    // entry to acceptImportedSignal() on the GUI thread. Reusing a stable
    // source key is idempotent across retries and process restarts.
    [[nodiscard]] core::Result<SignalEntry> persistGeneratedSignal(
        core::Signal signal,
        const std::string& stableSourceKey,
        std::string parametersJson = "{}",
        std::stop_token stopToken = {});

    [[nodiscard]] core::Result<repository::ImportPreview> previewCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {}) const;

    [[nodiscard]] core::Result<repository::ImportPreview> previewWav(
        const std::filesystem::path& sourcePath,
        std::stop_token stopToken = {}) const;

    [[nodiscard]] core::Result<std::size_t> importCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {});

    [[nodiscard]] core::Result<std::size_t> importWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options,
        std::stop_token stopToken = {});

    // These two functions do the slow file and database work without changing
    // entries(). A background worker can call them safely, then hand the
    // returned entry to acceptImportedSignal() on the UI thread.
    [[nodiscard]] core::Result<SignalEntry> loadCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {});

    [[nodiscard]] core::Result<SignalEntry> loadWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options,
        std::stop_token stopToken = {});

    // Script execution is declarative and may be replayed. These variants
    // register a new logical signal when needed or return the already
    // persisted signal for the same content and parsing options. They do not
    // mutate entries(), so a background script worker never races the UI
    // model; stage 7 can refresh the project list on the GUI thread.
    [[nodiscard]] core::Result<SignalEntry> openCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {});

    [[nodiscard]] core::Result<SignalEntry> openWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options,
        std::stop_token stopToken = {});

    // Returns no index when a late background result refers to a record that
    // has already been deleted. SQLite AUTOINCREMENT prevents record-id reuse.
    [[nodiscard]] std::optional<std::size_t> acceptImportedSignal(
        SignalEntry entry);

    // Performs persistent deletion and project-archive compensation without
    // touching entries(). It is safe to call on an I/O worker. The GUI thread
    // calls forgetSignal() only after receiving the terminal event.
    [[nodiscard]] core::Result<SignalRemovalOutcome>
    removePersistedSignal(std::int64_t recordId);

    [[nodiscard]] bool forgetSignal(std::int64_t recordId);

    [[nodiscard]] const std::vector<SignalEntry>& entries() const noexcept;

private:
    [[nodiscard]] core::Result<SignalEntry> registerImportedSignal(
        repository::FileImport fileImport,
        std::string parametersJson,
        std::string logicalOptionsJson,
        std::stop_token stopToken,
        bool reuseExisting);

    [[nodiscard]] core::Result<SignalEntry> loadCsvImpl(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken,
        bool reuseExisting);

    [[nodiscard]] core::Result<SignalEntry> loadWavImpl(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options,
        std::stop_token stopToken,
        bool reuseExisting);

    repository::FileRepository& fileRepository_;
    repository::SignalRepository& signalRepository_;
    // The archive promotion and database registration form one compensated
    // operation. Serializing imports prevents two I/O workers from deleting
    // an archive that the other worker is about to register.
    std::mutex importMutex_;
    std::unordered_set<std::int64_t> deletedRecordIds_;
    std::vector<SignalEntry> entries_;
};

}  // namespace signallab::app
