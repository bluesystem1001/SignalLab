#pragma once

#include "signallab/database/database_connection.hpp"
#include "signallab/repository/task_repository.hpp"

namespace signallab::database {

class SqliteTaskRepository final : public repository::TaskRepository {
public:
    explicit SqliteTaskRepository(DatabaseConnection& connection);

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

private:
    DatabaseConnection& connection_;
};

}  // namespace signallab::database
