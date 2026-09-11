#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace signallab::database::test {

class TemporaryProject {
public:
    explicit TemporaryProject(const std::string& purpose)
    {
        const auto uniqueNumber =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-" + purpose + "-" + std::to_string(uniqueNumber));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryProject()
    {
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    TemporaryProject(const TemporaryProject&) = delete;
    TemporaryProject& operator=(const TemporaryProject&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return root_ / "project.sqlite3";
    }

private:
    std::filesystem::path root_;
};

inline std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

}  // namespace signallab::database::test
