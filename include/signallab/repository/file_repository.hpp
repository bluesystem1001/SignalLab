#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <stop_token>
#include <vector>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/repository/signal_repository.hpp"

namespace signallab::repository {

enum class ImportFormat {
    Csv,
    Wav
};

enum class CsvDelimiter {
    Comma,
    Tab
};

struct CsvImportOptions {
    CsvDelimiter delimiter{CsvDelimiter::Comma};
    bool hasHeader{true};
    std::size_t valueColumn{1};
    std::optional<std::size_t> timeColumn{0};
    double sampleRateIfNoTimeColumn{48000.0};
    core::SignalMetadata metadata;
};

enum class StereoChannel {
    Left,
    Right,
    Average
};

struct WavImportOptions {
    StereoChannel channel{StereoChannel::Average};
    core::SignalMetadata metadata;
};

struct ImportPreview {
    std::filesystem::path sourcePath;
    ImportFormat format{ImportFormat::Csv};
    std::size_t sampleCount{0};
    std::size_t columnCount{0};
    std::size_t channelCount{0};
    std::optional<double> sampleRate;
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> rows;
};

struct FileImport {
    SignalSourceType sourceType;
    std::string contentHash;
    std::filesystem::path sourcePath;
    std::filesystem::path storedRelativePath;
    std::uint64_t byteSize;
    bool createdStoredFile;
    core::Signal signal;
};

class FileRepository {
public:
    virtual ~FileRepository() = default;

    [[nodiscard]] virtual core::Result<ImportPreview> previewCsv(
        const std::filesystem::path& sourcePath,
        const CsvImportOptions& options,
        std::stop_token stopToken = {}) const = 0;

    [[nodiscard]] virtual core::Result<ImportPreview> previewWav(
        const std::filesystem::path& sourcePath,
        std::stop_token stopToken = {}) const = 0;

    [[nodiscard]] virtual core::Result<FileImport> importCsv(
        const std::filesystem::path& sourcePath,
        const CsvImportOptions& options,
        std::stop_token stopToken = {}) = 0;

    [[nodiscard]] virtual core::Result<FileImport> importWav(
        const std::filesystem::path& sourcePath,
        const WavImportOptions& options,
        std::stop_token stopToken = {}) = 0;

    [[nodiscard]] virtual core::Result<bool> validateStoredFile(
        const std::filesystem::path& storedRelativePath,
        const std::string& expectedHash,
        std::stop_token stopToken = {}) const = 0;

    [[nodiscard]] virtual core::Result<bool> removeStoredFile(
        const std::filesystem::path& storedRelativePath,
        std::stop_token stopToken = {}) = 0;
};

}  // namespace signallab::repository
