#pragma once

#include <filesystem>
#include <stop_token>
#include <vector>

#include "signallab/repository/backup_repository.hpp"

namespace signallab::database {

class ProjectBackupSource final : public repository::BackupSource {
public:
    explicit ProjectBackupSource(std::filesystem::path projectRoot);

    [[nodiscard]] core::Result<std::vector<repository::BackupFile>> snapshot(
        const std::filesystem::path& stagingDirectory,
        std::stop_token stopToken = {}) override;
    [[nodiscard]] core::Result<bool> verify(
        const std::filesystem::path& directory,
        const std::vector<repository::BackupFile>& files,
        std::stop_token stopToken = {}) override;

private:
    std::filesystem::path projectRoot_;
};

}
