#pragma once

#include <filesystem>

#include "signallab/repository/backup_repository.hpp"

namespace signallab::io {

struct BackupSettings {
    repository::BackupConnectionOptions connection;
    bool rememberPassword{false};
};

[[nodiscard]] std::filesystem::path defaultBackupSettingsPath();
[[nodiscard]] core::Result<BackupSettings> loadBackupSettings(
    const std::filesystem::path& path);
[[nodiscard]] core::Result<bool> saveBackupSettings(
    const std::filesystem::path& path, const BackupSettings& settings);

}
