#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/io/import_types.hpp"

namespace signallab::app {

struct SignalEntry {
    std::string displayName;
    std::string hash;
    std::filesystem::path sourcePath;
    std::filesystem::path storedRelativePath;
    core::Signal signal;
};

class ImportService {
public:
    explicit ImportService(std::filesystem::path projectRoot);

    [[nodiscard]] std::size_t addGeneratedSignal(core::Signal signal);

    [[nodiscard]] core::Result<io::ImportPreview> previewCsv(
        const std::filesystem::path& sourcePath,
        const io::CsvImportOptions& options) const;

    [[nodiscard]] core::Result<io::ImportPreview> previewWav(
        const std::filesystem::path& sourcePath) const;

    [[nodiscard]] core::Result<std::size_t> importCsv(
        const std::filesystem::path& sourcePath,
        const io::CsvImportOptions& options);

    [[nodiscard]] core::Result<std::size_t> importWav(
        const std::filesystem::path& sourcePath,
        const io::WavImportOptions& options);

    [[nodiscard]] const std::vector<SignalEntry>& entries() const noexcept;

private:
    [[nodiscard]] core::Result<bool> isDuplicate(
        const std::string& hash,
        const std::filesystem::path& storedRelativePath) const;

    [[nodiscard]] core::Result<std::size_t> storeImportedSignal(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& snapshotPath,
        std::string hash,
        std::filesystem::path storedRelativePath,
        core::Signal signal);

    std::filesystem::path projectRoot_;
    std::vector<SignalEntry> entries_;
};

}  // namespace signallab::app
