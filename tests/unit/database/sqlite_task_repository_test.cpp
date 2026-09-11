#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/signal.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"
#include "signallab/repository/task_repository.hpp"
#include "test_database_helpers.hpp"

namespace signallab::database {
namespace {

repository::AnalysisTaskStart makeTask(std::string signalName)
{
    return repository::AnalysisTaskStart{
        std::nullopt,
        std::move(signalName),
        "synchronous-analysis",
        "analysis-service/1.0.0|fft=fft-radix2/1.0.0",
        R"({"window_type":"hann"})",
        std::string(64, 'c'),
    };
}

const repository::AnalysisTaskRecord* findTask(
    const std::vector<repository::AnalysisTaskRecord>& tasks,
    std::int64_t taskId)
{
    const auto item = std::find_if(
        tasks.begin(),
        tasks.end(),
        [taskId](const repository::AnalysisTaskRecord& task) {
            return task.id == taskId;
        });
    if (item == tasks.end()) {
        return nullptr;
    }
    return &*item;
}

TEST(SqliteTaskRepositoryTest, PersistsSuccessfulHistoryFeaturesAndStableHash)
{
    test::TemporaryProject project("task-success");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteTaskRepository repository(connection);

    const std::vector<repository::FeatureValue> features{
        {"rms", 0.5, "V"},
        {"mean", 0.0, "V"},
        {"snr_db", std::numeric_limits<double>::infinity(), "dB"},
        {"floor", -std::numeric_limits<double>::infinity(), "dB"},
    };

    const auto firstTask = repository.createTask(makeTask("First signal"));
    ASSERT_TRUE(firstTask);
    const auto firstCompletion = repository.completeTask(
        firstTask.value(), features);
    ASSERT_TRUE(firstCompletion);
    EXPECT_EQ(
        firstCompletion.value(),
        "42d7cc3c71da83eee307bb42d220a1d"
        "6156239d8adadfce13d1a526127344e79");

    const auto secondTask = repository.createTask(makeTask("Second signal"));
    ASSERT_TRUE(secondTask);
    const std::vector<repository::FeatureValue> reversedFeatures{
        features.rbegin(), features.rend()};
    const auto secondCompletion = repository.completeTask(
        secondTask.value(), reversedFeatures);
    ASSERT_TRUE(secondCompletion);
    EXPECT_EQ(secondCompletion.value(), firstCompletion.value());

    const auto thirdTask = repository.createTask(makeTask("Third signal"));
    ASSERT_TRUE(thirdTask);
    auto changedFeatures = features;
    changedFeatures.front().value = 0.75;
    const auto thirdCompletion = repository.completeTask(
        thirdTask.value(),
        changedFeatures);
    ASSERT_TRUE(thirdCompletion);
    EXPECT_NE(thirdCompletion.value(), firstCompletion.value());

    const auto tasks = repository.listTasks(10);
    ASSERT_TRUE(tasks);
    const auto* storedTask = findTask(tasks.value(), firstTask.value());
    ASSERT_NE(storedTask, nullptr);
    EXPECT_EQ(storedTask->status, repository::AnalysisTaskStatus::Succeeded);
    EXPECT_EQ(storedTask->parametersJson, R"({"window_type":"hann"})");
    EXPECT_EQ(storedTask->inputHash, std::string(64, 'c'));
    EXPECT_EQ(storedTask->outputHash, firstCompletion.value());
    EXPECT_FALSE(storedTask->finishedUtc.empty());

    const auto storedFeatures = repository.listFeatureResults(firstTask.value());
    ASSERT_TRUE(storedFeatures);
    ASSERT_EQ(storedFeatures.value().size(), 4U);
    const auto infinityFeature = std::find_if(
        storedFeatures.value().begin(),
        storedFeatures.value().end(),
        [](const repository::FeatureResultRecord& feature) {
            return feature.name == "snr_db";
        });
    ASSERT_NE(infinityFeature, storedFeatures.value().end());
    EXPECT_FALSE(infinityFeature->numericValue.has_value());
    EXPECT_EQ(infinityFeature->valueText, "inf");

    const auto negativeInfinityFeature = std::find_if(
        storedFeatures.value().begin(),
        storedFeatures.value().end(),
        [](const repository::FeatureResultRecord& feature) {
            return feature.name == "floor";
        });
    ASSERT_NE(negativeInfinityFeature, storedFeatures.value().end());
    EXPECT_FALSE(negativeInfinityFeature->numericValue.has_value());
    EXPECT_EQ(negativeInfinityFeature->valueText, "-inf");
}

TEST(SqliteTaskRepositoryTest, RejectsAHashThatDoesNotMatchTheSignalRecord)
{
    test::TemporaryProject project("task-input-mismatch");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    auto signal = core::Signal::create(
        {0.0, 1.0, 0.0, -1.0},
        1'000.0,
        core::SignalMetadata{"Task input", "V", "unit test"});
    ASSERT_TRUE(signal);
    SqliteSignalRepository signalRepository(connection);
    auto storedSignal = signalRepository.saveSignal(
        repository::SignalRegistration{
            repository::SignalSourceType::Generated,
            "task-input-signal",
            {},
            "{}",
            std::nullopt,
            std::move(signal).value()});
    ASSERT_TRUE(storedSignal);

    auto task = makeTask("Mismatched input");
    task.signalRecordId = storedSignal.value().recordId;
    task.inputHash = std::string(64, 'd');
    SqliteTaskRepository taskRepository(connection);

    const auto taskResult = taskRepository.createTask(task);

    ASSERT_FALSE(taskResult);
    EXPECT_EQ(taskResult.error().code, core::ErrorCode::InvalidParameter);
    const auto history = taskRepository.listTasks(10);
    ASSERT_TRUE(history);
    EXPECT_TRUE(history.value().empty());
}

TEST(SqliteTaskRepositoryTest, RejectsAnUppercaseInputHash)
{
    test::TemporaryProject project("task-uppercase-hash");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteTaskRepository repository(connection);

    auto task = makeTask("Uppercase hash");
    task.inputHash = std::string(64, 'C');
    const auto taskResult = repository.createTask(task);

    ASSERT_FALSE(taskResult);
    EXPECT_EQ(taskResult.error().code, core::ErrorCode::InvalidParameter);
    const auto history = repository.listTasks(10);
    ASSERT_TRUE(history);
    EXPECT_TRUE(history.value().empty());
}

TEST(SqliteTaskRepositoryTest, RollsBackPartialFeaturesAndMarksTaskFailed)
{
    test::TemporaryProject project("task-rollback");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteTaskRepository repository(connection);

    const auto task = repository.createTask(makeTask("Failure injection"));
    ASSERT_TRUE(task);
    const std::vector<repository::FeatureValue> duplicateFeatures{
        {"mean", 1.0, "V"},
        {"mean", 2.0, "V"},
    };

    const auto completion = repository.completeTask(
        task.value(), duplicateFeatures);

    EXPECT_FALSE(completion);
    const auto features = repository.listFeatureResults(task.value());
    ASSERT_TRUE(features);
    EXPECT_TRUE(features.value().empty());

    const auto tasks = repository.listTasks(10);
    ASSERT_TRUE(tasks);
    const auto* failedTask = findTask(tasks.value(), task.value());
    ASSERT_NE(failedTask, nullptr);
    EXPECT_EQ(failedTask->status, repository::AnalysisTaskStatus::Failed);
    EXPECT_FALSE(failedTask->errorMessage.empty());
    EXPECT_TRUE(failedTask->outputHash.empty());
}

TEST(SqliteTaskRepositoryTest, RejectsNaNWithoutLeavingFeatureRows)
{
    test::TemporaryProject project("task-nan-feature");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteTaskRepository repository(connection);

    const auto task = repository.createTask(makeTask("NaN feature"));
    ASSERT_TRUE(task);
    const std::vector<repository::FeatureValue> features{
        {"mean", 1.0, "V"},
        {"invalid", std::numeric_limits<double>::quiet_NaN(), "V"},
    };

    const auto completion = repository.completeTask(task.value(), features);

    ASSERT_FALSE(completion);
    EXPECT_EQ(completion.error().code, core::ErrorCode::InvalidParameter);
    const auto storedFeatures = repository.listFeatureResults(task.value());
    ASSERT_TRUE(storedFeatures);
    EXPECT_TRUE(storedFeatures.value().empty());

    const auto tasks = repository.listTasks(10);
    ASSERT_TRUE(tasks);
    const auto* failedTask = findTask(tasks.value(), task.value());
    ASSERT_NE(failedTask, nullptr);
    EXPECT_EQ(failedTask->status, repository::AnalysisTaskStatus::Failed);
    EXPECT_FALSE(failedTask->errorMessage.empty());
    EXPECT_TRUE(failedTask->outputHash.empty());
}

TEST(SqliteTaskRepositoryTest, MarksRunningTasksInterruptedAfterRestart)
{
    test::TemporaryProject project("task-interrupted");
    std::int64_t taskId = 0;

    {
        DatabaseConnection connection(project.databasePath());
        ASSERT_TRUE(connection.open());
        MigrationRunner runner(connection);
        ASSERT_TRUE(runner.run(test::migrationsDirectory()));
        SqliteTaskRepository repository(connection);
        const auto task = repository.createTask(makeTask("Interrupted signal"));
        ASSERT_TRUE(task);
        taskId = task.value();
    }

    DatabaseConnection restartedConnection(project.databasePath());
    ASSERT_TRUE(restartedConnection.open());
    MigrationRunner restartedRunner(restartedConnection);
    ASSERT_TRUE(restartedRunner.run(test::migrationsDirectory()));
    SqliteTaskRepository restartedRepository(restartedConnection);

    const auto interrupted = restartedRepository.interruptRunningTasks();
    ASSERT_TRUE(interrupted);
    EXPECT_EQ(interrupted.value(), 1U);

    const auto tasks = restartedRepository.listTasks(10);
    ASSERT_TRUE(tasks);
    const auto* restoredTask = findTask(tasks.value(), taskId);
    ASSERT_NE(restoredTask, nullptr);
    EXPECT_EQ(
        restoredTask->status,
        repository::AnalysisTaskStatus::Interrupted);
    EXPECT_FALSE(restoredTask->finishedUtc.empty());
}

}  // namespace
}  // namespace signallab::database
