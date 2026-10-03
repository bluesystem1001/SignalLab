#pragma once

#include <filesystem>
#include <memory>

#include "signallab/repository/script_repository.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "signallab/repository/task_repository.hpp"

namespace signallab::database {

// Serializes every SQLite operation on one dedicated thread. The Qt SQL
// connection and the concrete repositories are created and destroyed inside
// that thread, so callers may safely use this proxy from other threads.
class DatabaseExecutor final :
    public repository::SignalRepository,
    public repository::TaskRepository,
    public repository::ScriptRepository {
public:
    DatabaseExecutor(
        std::filesystem::path databasePath,
        std::filesystem::path migrationsDirectory);
    ~DatabaseExecutor() override;

    DatabaseExecutor(const DatabaseExecutor&) = delete;
    DatabaseExecutor& operator=(const DatabaseExecutor&) = delete;
    DatabaseExecutor(DatabaseExecutor&&) = delete;
    DatabaseExecutor& operator=(DatabaseExecutor&&) = delete;

    // Repeated calls while running succeed without creating another thread.
    [[nodiscard]] core::Result<bool> start();

    // Stops accepting commands, drains commands already accepted, then closes
    // SQLite in its owning thread. Calling shutdown again is safe.
    [[nodiscard]] core::Result<bool> shutdown();

    [[nodiscard]] bool isRunning() const noexcept;

    [[nodiscard]] core::Result<repository::StoredSignal> saveSignal(
        repository::SignalRegistration registration) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findBySourceKey(const std::string& sourceKey) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findByRecordId(
        std::int64_t recordId,
        std::stop_token stopToken = {}) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findByFileHash(const std::string& fileHash) override;

    [[nodiscard]] core::Result<std::vector<repository::StoredSignal>>
    listSignals() override;

    [[nodiscard]] core::Result<repository::SignalRemoval> removeSignal(
        std::int64_t recordId) override;

    [[nodiscard]] core::Result<std::int64_t> createTask(
        const repository::AnalysisTaskStart& task) override;

    [[nodiscard]] core::Result<bool> startTask(
        std::int64_t taskId) override;

    [[nodiscard]] core::Result<std::string> completeTask(
        std::int64_t taskId,
        const std::vector<repository::FeatureValue>& features) override;

    [[nodiscard]] core::Result<bool> failTask(
        std::int64_t taskId,
        const std::string& errorMessage) override;

    [[nodiscard]] core::Result<bool> cancelTask(
        std::int64_t taskId,
        const std::string& reason) override;

    [[nodiscard]] core::Result<std::size_t> interruptRunningTasks() override;

    [[nodiscard]] core::Result<std::vector<repository::AnalysisTaskRecord>>
    listTasks(std::size_t maximumCount) override;

    [[nodiscard]] core::Result<std::vector<repository::FeatureResultRecord>>
    listFeatureResults(std::int64_t taskId) override;

    [[nodiscard]] core::Result<bool> removeTask(
        std::int64_t taskId) override;

    [[nodiscard]] core::Result<std::int64_t> createScriptHistory(
        std::string scriptText) override;

    [[nodiscard]] core::Result<bool> finishScriptHistory(
        std::int64_t historyId,
        repository::ScriptHistoryFinish finish) override;

    [[nodiscard]] core::Result<std::size_t>
    interruptRunningScripts() override;

    [[nodiscard]] core::Result<std::vector<repository::ScriptHistoryRecord>>
    listScriptHistory(std::size_t maximumCount) override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

}  // namespace signallab::database
