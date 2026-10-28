#pragma once

#include "signallab/repository/backup_repository.hpp"

namespace signallab::database {

class MariaDbBackupRepository final : public repository::BackupRepository {
public:
    [[nodiscard]] core::Result<std::string> testConnection(
        const repository::BackupConnectionOptions& options) override;
    [[nodiscard]] core::Result<repository::BackupRecord> upload(
        const repository::BackupConnectionOptions& options,
        repository::BackupRecord record,
        const std::vector<repository::BackupFile>& files,
        std::stop_token stopToken = {},
        repository::BackupProgress progress = {}) override;
    [[nodiscard]] core::Result<std::vector<repository::BackupRecord>> list(
        const repository::BackupConnectionOptions& options) override;
    [[nodiscard]] core::Result<std::vector<repository::BackupFile>> download(
        const repository::BackupConnectionOptions& options,
        const std::string& backupId,
        const std::filesystem::path& stagingDirectory,
        std::stop_token stopToken = {},
        repository::BackupProgress progress = {}) override;
};

}
