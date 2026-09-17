#include <QSqlQuery>

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
    const auto firstStart = repository.startTask(firstTask.value());
    ASSERT_TRUE(firstStart);
    ASSERT_TRUE(firstStart.value());
    const auto firstCompletion = repository.completeTask(
        firstTask.value(), features);
    ASSERT_TRUE(firstCompletion);
    EXPECT_EQ(
        firstCompletion.value(),
        "42d7cc3c71da83eee307bb42d220a1d"
        "6156239d8adadfce13d1a526127344e79");

    const auto secondTask = repository.createTask(makeTask("Second signal"));
    ASSERT_TRUE(secondTask);
    const auto secondStart = repository.startTask(secondTask.value());
    ASSERT_TRUE(secondStart);
    ASSERT_TRUE(secondStart.value());
    const std::vector<repository::FeatureValue> reversedFeatures{
        features.rbegin(), features.rend()};
    const auto secondCompletion = repository.completeTask(
        secondTask.value(), reversedFeatures);
    ASSERT_TRUE(secondCompletion);
    EXPECT_EQ(secondCompletion.value(), firstCompletion.value());

    const auto thirdTask = repository.createTask(makeTask("Third signal"));
    ASSERT_TRUE(thirdTask);
    const auto thirdStart = repository.startTask(thirdTask.value());
    ASSERT_TRUE(thirdStart);
    ASSERT_TRUE(thirdStart.value());
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
    const auto start = repository.startTask(task.value());
    ASSERT_TRUE(start);
    ASSERT_TRUE(start.value());
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
    const auto start = repository.startTask(task.value());
    ASSERT_TRUE(start);
    ASSERT_TRUE(start.value());
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

TEST(SqliteTaskRepositoryTest, MarksPendingAndRunningTasksInterruptedAfterRestart)
{
    test::TemporaryProject project("task-interrupted");
    std::int64_t pendingTaskId = 0;
    std::int64_t runningTaskId = 0;

    {
        DatabaseConnection connection(project.databasePath());
        ASSERT_TRUE(connection.open());
        MigrationRunner runner(connection);
        ASSERT_TRUE(runner.run(test::migrationsDirectory()));
        SqliteTaskRepository repository(connection);
        const auto pendingTask = repository.createTask(
            makeTask("Pending signal"));
        ASSERT_TRUE(pendingTask);
        pendingTaskId = pendingTask.value();

        const auto runningTask = repository.createTask(
            makeTask("Running signal"));
        ASSERT_TRUE(runningTask);
        runningTaskId = runningTask.value();
        const auto start = repository.startTask(runningTaskId);
        ASSERT_TRUE(start);
        ASSERT_TRUE(start.value());
    }

    DatabaseConnection restartedConnection(project.databasePath());
    ASSERT_TRUE(restartedConnection.open());
    MigrationRunner restartedRunner(restartedConnection);
    ASSERT_TRUE(restartedRunner.run(test::migrationsDirectory()));
    SqliteTaskRepository restartedRepository(restartedConnection);

    const auto interrupted = restartedRepository.interruptRunningTasks();
    ASSERT_TRUE(interrupted);
    EXPECT_EQ(interrupted.value(), 2U);

    const auto tasks = restartedRepository.listTasks(10);
    ASSERT_TRUE(tasks);
    const auto* restoredPending = findTask(tasks.value(), pendingTaskId);
    ASSERT_NE(restoredPending, nullptr);
    EXPECT_EQ(
        restoredPending->status,
        repository::AnalysisTaskStatus::Interrupted);
    EXPECT_TRUE(restoredPending->startedUtc.empty());
    EXPECT_FALSE(restoredPending->finishedUtc.empty());

    const auto* restoredRunning = findTask(tasks.value(), runningTaskId);
    ASSERT_NE(restoredRunning, nullptr);
    EXPECT_EQ(
        restoredRunning->status,
        repository::AnalysisTaskStatus::Interrupted);
    EXPECT_FALSE(restoredRunning->startedUtc.empty());
    EXPECT_FALSE(restoredRunning->finishedUtc.empty());
}

TEST(SqliteTaskRepositoryTest, PersistsPendingStartAndCancellationTransitions)
{
    test::TemporaryProject project("task-cancel");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteTaskRepository repository(connection);

    const auto pendingTask = repository.createTask(makeTask("Cancel pending"));
    ASSERT_TRUE(pendingTask);
    auto history = repository.listTasks(10);
    ASSERT_TRUE(history);
    const auto* pending = findTask(history.value(), pendingTask.value());
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->status, repository::AnalysisTaskStatus::Pending);
    EXPECT_TRUE(pending->startedUtc.empty());
    EXPECT_TRUE(pending->finishedUtc.empty());

    const auto cancelPending = repository.cancelTask(
        pendingTask.value(), "");
    ASSERT_TRUE(cancelPending);
    EXPECT_TRUE(cancelPending.value());

    const auto runningTask = repository.createTask(makeTask("Cancel running"));
    ASSERT_TRUE(runningTask);
    const auto start = repository.startTask(runningTask.value());
    ASSERT_TRUE(start);
    EXPECT_TRUE(start.value());
    const auto secondStart = repository.startTask(runningTask.value());
    ASSERT_TRUE(secondStart);
    EXPECT_FALSE(secondStart.value());

    const auto cancelRunning = repository.cancelTask(
        runningTask.value(), "Cancelled by the test.");
    ASSERT_TRUE(cancelRunning);
    EXPECT_TRUE(cancelRunning.value());
    const auto secondCancel = repository.cancelTask(
        runningTask.value(), "A repeated request.");
    ASSERT_TRUE(secondCancel);
    EXPECT_FALSE(secondCancel.value());

    QSqlQuery logQuery(connection.database());
    ASSERT_TRUE(logQuery.prepare(
        "SELECT detail, typeof(detail) FROM operation_log "
        "WHERE operation IN ('START_TASK', 'CANCEL_TASK') "
        "ORDER BY id"));
    ASSERT_TRUE(logQuery.exec());
    int stateLogCount = 0;
    while (logQuery.next()) {
        EXPECT_FALSE(logQuery.value(0).isNull());
        EXPECT_EQ(logQuery.value(1).toString(), "text");
        ++stateLogCount;
    }
    EXPECT_EQ(stateLogCount, 3);

    history = repository.listTasks(10);
    ASSERT_TRUE(history);
    for (const std::int64_t taskId : {
             pendingTask.value(), runningTask.value()}) {
        const auto* cancelled = findTask(history.value(), taskId);
        ASSERT_NE(cancelled, nullptr);
        EXPECT_EQ(
            cancelled->status,
            repository::AnalysisTaskStatus::Cancelled);
        EXPECT_TRUE(cancelled->outputHash.empty());
        EXPECT_FALSE(cancelled->finishedUtc.empty());

        const auto features = repository.listFeatureResults(taskId);
        ASSERT_TRUE(features);
        EXPECT_TRUE(features.value().empty());
    }
}

}  // namespace
}  // namespace signallab::database
