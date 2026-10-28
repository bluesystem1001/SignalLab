#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::repository {

struct BackupConnectionOptions {
    std::string host{"127.0.0.1"};
    unsigned int port{3306};
    std::string username{"root"};
    std::string password;
    std::string databaseName{"signallab_backup"};
};

struct BackupFile {
    std::string relativePath;
    std::filesystem::path localPath;
    std::uint64_t byteSize{0};
    std::string sha256;
};

struct BackupRecord {
    std::string id;
    std::string createdUtc;
    std::string label;
    std::string sourceName;
    std::uint64_t totalBytes{0};
    std::uint64_t fileCount{0};
};

using BackupProgress = std::function<void(std::uint64_t completed, std::uint64_t total)>;

class BackupRepository {
public:
    virtual ~BackupRepository() = default;
    [[nodiscard]] virtual core::Result<std::string> testConnection(
        const BackupConnectionOptions& options) = 0;
    [[nodiscard]] virtual core::Result<BackupRecord> upload(
        const BackupConnectionOptions& options,
        BackupRecord record,
        const std::vector<BackupFile>& files,
        std::stop_token stopToken = {},
        BackupProgress progress = {}) = 0;
    [[nodiscard]] virtual core::Result<std::vector<BackupRecord>> list(
        const BackupConnectionOptions& options) = 0;
    [[nodiscard]] virtual core::Result<std::vector<BackupFile>> download(
        const BackupConnectionOptions& options,
        const std::string& backupId,
        const std::filesystem::path& stagingDirectory,
        std::stop_token stopToken = {},
        BackupProgress progress = {}) = 0;
};

class BackupSource {
public:
    virtual ~BackupSource() = default;
    [[nodiscard]] virtual core::Result<std::vector<BackupFile>> snapshot(
        const std::filesystem::path& stagingDirectory,
        std::stop_token stopToken = {}) = 0;
    [[nodiscard]] virtual core::Result<bool> verify(
        const std::filesystem::path& directory,
        const std::vector<BackupFile>& files,
        std::stop_token stopToken = {}) = 0;
};

}
