#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
        const repository::CsvImportOptions& options) const;

    [[nodiscard]] core::Result<repository::ImportPreview> previewWav(
        const std::filesystem::path& sourcePath) const;

    [[nodiscard]] core::Result<std::size_t> importCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options);

    [[nodiscard]] core::Result<std::size_t> importWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options);

    [[nodiscard]] const std::vector<SignalEntry>& entries() const noexcept;

private:
    [[nodiscard]] core::Result<std::size_t> registerImportedSignal(
        repository::FileImport fileImport,
        std::string parametersJson,
        std::string logicalOptionsJson);

    repository::FileRepository& fileRepository_;
    repository::SignalRepository& signalRepository_;
    std::vector<SignalEntry> entries_;
};

}  // namespace signallab::app
