#pragma once

#include <filesystem>
#include <string>

#include "signallab/repository/file_repository.hpp"

namespace signallab::io {

class FilesystemFileRepository final : public repository::FileRepository {
public:
    explicit FilesystemFileRepository(std::filesystem::path projectRoot);

    [[nodiscard]] core::Result<repository::ImportPreview> previewCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options) const override;

    [[nodiscard]] core::Result<repository::ImportPreview> previewWav(
        const std::filesystem::path& sourcePath) const override;

    [[nodiscard]] core::Result<repository::FileImport> importCsv(
        const std::filesystem::path& sourcePath,
        const repository::CsvImportOptions& options) override;

    [[nodiscard]] core::Result<repository::FileImport> importWav(
        const std::filesystem::path& sourcePath,
        const repository::WavImportOptions& options) override;

    [[nodiscard]] core::Result<bool> validateStoredFile(
        const std::filesystem::path& storedRelativePath,
        const std::string& expectedHash) const override;

    [[nodiscard]] core::Result<bool> removeStoredFile(
        const std::filesystem::path& storedRelativePath) override;

private:
    std::filesystem::path projectRoot_;
};

}  // namespace signallab::io
