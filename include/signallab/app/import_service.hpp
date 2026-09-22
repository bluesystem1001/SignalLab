#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
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

    [[nodiscard]] std::size_t acceptImportedSignal(SignalEntry entry);

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
    std::vector<SignalEntry> entries_;
};

}  // namespace signallab::app
