#pragma once

#include <filesystem>
#include <string>
#include <stop_token>

#include "signallab/repository/file_repository.hpp"

namespace signallab::io {

class FilesystemFileRepository final : public repository::FileRepository {
public:
    explicit FilesystemFileRepository(std::filesystem::path projectRoot);

    [[nodiscard]] core::Result<repository::ImportPreview> previewCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {}) const override;

    [[nodiscard]] core::Result<repository::ImportPreview> previewWav(
        const std::filesystem::path& sourcePath,
        std::stop_token stopToken = {}) const override;

    [[nodiscard]] core::Result<repository::FileImport> importCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options,
        std::stop_token stopToken = {}) override;

    [[nodiscard]] core::Result<repository::FileImport> importWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options,
        std::stop_token stopToken = {}) override;

    [[nodiscard]] core::Result<bool> validateStoredFile(
        const std::filesystem::path& storedRelativePath,
        const std::string& expectedHash,
        std::stop_token stopToken = {}) const override;

    [[nodiscard]] core::Result<bool> removeStoredFile(
        const std::filesystem::path& storedRelativePath,
        std::stop_token stopToken = {}) override;

private:
    std::filesystem::path projectRoot_;
};

}  // namespace signallab::io
