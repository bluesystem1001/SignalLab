#pragma once

#include <memory>

#include "signallab/repository/backup_repository.hpp"

namespace signallab::app {

class BackupService {
public:
    BackupService(std::shared_ptr<repository::BackupSource> source,
                  std::shared_ptr<repository::BackupRepository> repository);
    [[nodiscard]] core::Result<std::string> testConnection(
        const repository::BackupConnectionOptions& options);
    [[nodiscard]] core::Result<std::vector<repository::BackupRecord>> list(
        const repository::BackupConnectionOptions& options);
    [[nodiscard]] core::Result<repository::BackupRecord> backup(
        const repository::BackupConnectionOptions& options,
        const std::string& label,
        std::stop_token stopToken = {},
        repository::BackupProgress progress = {});
    [[nodiscard]] core::Result<std::filesystem::path> restoreCopy(
        const repository::BackupConnectionOptions& options,
        const std::string& backupId,
        const std::filesystem::path& destination,
        std::stop_token stopToken = {},
        repository::BackupProgress progress = {});

private:
    std::shared_ptr<repository::BackupSource> source_;
    std::shared_ptr<repository::BackupRepository> repository_;
};

}
