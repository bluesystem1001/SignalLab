#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::repository {

enum class AnalysisTaskStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
    Interrupted,
    Cancelled
};

struct AnalysisTaskStart {
    std::optional<std::int64_t> signalRecordId;
    std::string signalName;
    std::string algorithmName;
    std::string algorithmVersion;
    std::string parametersJson;
    std::string inputHash;
};

struct FeatureValue {
    std::string name;
    double value;
    std::string unit;
};

struct FeatureResultRecord {
    std::int64_t id;
    std::int64_t taskId;
    std::string name;
    std::optional<double> numericValue;
    std::string valueText;
    std::string unit;
};

struct AnalysisTaskRecord {
    std::int64_t id;
    std::optional<std::int64_t> signalRecordId;
    std::string signalName;
    AnalysisTaskStatus status;
    std::string algorithmName;
    std::string algorithmVersion;
    std::string parametersJson;
    std::string inputHash;
    std::string outputHash;
    std::string createdUtc;
    std::string startedUtc;
    std::string finishedUtc;
    std::string errorMessage;
};

class TaskRepository {
public:
    virtual ~TaskRepository() = default;

    [[nodiscard]] virtual core::Result<std::int64_t> createTask(
        const AnalysisTaskStart& task) = 0;

    // Starts one queued task. Returns false when the task is no longer
    // PENDING, so competing start/cancel requests have one clear winner.
    [[nodiscard]] virtual core::Result<bool> startTask(
        std::int64_t taskId) = 0;

    // Stores all feature rows and marks the task SUCCEEDED in one transaction.
    // If any row fails, implementations must roll back and mark the task FAILED.
    [[nodiscard]] virtual core::Result<std::string> completeTask(
        std::int64_t taskId,
        const std::vector<FeatureValue>& features) = 0;

    [[nodiscard]] virtual core::Result<bool> failTask(
        std::int64_t taskId,
        const std::string& errorMessage) = 0;

    // Cancels a queued or running task and removes any unfinished features.
    [[nodiscard]] virtual core::Result<bool> cancelTask(
        std::int64_t taskId,
        const std::string& reason) = 0;

    // Kept under its stage-4 name for source compatibility. Stage 5 also
    // interrupts PENDING tasks left behind by an abnormal exit.
    [[nodiscard]] virtual core::Result<std::size_t> interruptRunningTasks() = 0;

    [[nodiscard]] virtual core::Result<std::vector<AnalysisTaskRecord>>
    listTasks(std::size_t maximumCount) = 0;

    [[nodiscard]] virtual core::Result<std::vector<FeatureResultRecord>>
    listFeatureResults(std::int64_t taskId) = 0;
};

}  // namespace signallab::repository
