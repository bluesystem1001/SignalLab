#include <QSqlQuery>
#include <QVariant>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_script_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"
#include "test_database_helpers.hpp"

namespace signallab::database {
namespace {

TEST(MigrationRunnerTest, CreatesTheSchemaOnceAndConfiguresTheConnection)
{
    test::TemporaryProject project("migration");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());

    MigrationRunner runner(connection);
    const auto firstRun = runner.run(test::migrationsDirectory());
    const auto secondRun = runner.run(test::migrationsDirectory());

    ASSERT_TRUE(firstRun);
    EXPECT_EQ(firstRun.value(), 2U);
    ASSERT_TRUE(secondRun);
    EXPECT_EQ(secondRun.value(), 0U);

    QSqlQuery query(connection.database());
    ASSERT_TRUE(query.exec("PRAGMA foreign_keys"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toInt(), 1);

    ASSERT_TRUE(query.exec("PRAGMA journal_mode"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toString().toLower(), "wal");

    ASSERT_TRUE(query.exec("PRAGMA busy_timeout"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toInt(), 5000);

    ASSERT_TRUE(query.exec(
        "SELECT name FROM sqlite_master WHERE type = 'table'"));
    std::set<std::string> tableNames;
    while (query.next()) {
        tableNames.insert(query.value(0).toString().toStdString());
    }

    const std::set<std::string> requiredTables{
        "analysis_task",
        "feature_result",
        "file_version",
        "operation_log",
        "schema_migration",
        "script_history",
        "signal_record",
        "signal_source",
    };
    for (const auto& tableName : requiredTables) {
        EXPECT_TRUE(tableNames.contains(tableName)) << tableName;
    }
}

TEST(MigrationRunnerTest, StageFiveMigrationPreservesTasksAndFeatures)
{
    test::TemporaryProject project("migration-stage-five");
    const auto migrations = project.root() / "migrations";
    std::filesystem::create_directories(migrations);
    std::filesystem::copy_file(
        test::migrationsDirectory() / "001_init.sql",
        migrations / "001_init.sql");

    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    const auto initialMigration = runner.run(migrations);
    ASSERT_TRUE(initialMigration);
    ASSERT_EQ(initialMigration.value(), 1U);

    SqliteTaskRepository repository(connection);
    const repository::AnalysisTaskStart taskStart{
        std::nullopt,
        "Preserved task",
        "migration-test",
        "1.0.0",
        "{}",
        std::string(64, 'a')};
    const auto task = repository.createTask(taskStart);
    ASSERT_TRUE(task);
    const auto started = repository.startTask(task.value());
    ASSERT_TRUE(started);
    ASSERT_TRUE(started.value());
    const auto completed = repository.completeTask(
        task.value(), {{"mean", 1.25, "V"}});
    ASSERT_TRUE(completed);

    std::filesystem::copy_file(
        test::migrationsDirectory() / "002_task_scheduler.sql",
        migrations / "002_task_scheduler.sql");
    const auto stageFiveMigration = runner.run(migrations);
    ASSERT_TRUE(stageFiveMigration);
    EXPECT_EQ(stageFiveMigration.value(), 1U);

    const auto history = repository.listTasks(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().id, task.value());
    EXPECT_EQ(
        history.value().front().status,
        repository::AnalysisTaskStatus::Succeeded);
    EXPECT_EQ(history.value().front().outputHash, completed.value());

    const auto features = repository.listFeatureResults(task.value());
    ASSERT_TRUE(features);
    ASSERT_EQ(features.value().size(), 1U);
    EXPECT_EQ(features.value().front().name, "mean");
    ASSERT_TRUE(features.value().front().numericValue.has_value());
    EXPECT_DOUBLE_EQ(*features.value().front().numericValue, 1.25);

    const auto cancellable = repository.createTask(taskStart);
    ASSERT_TRUE(cancellable);
    const auto cancelled = repository.cancelTask(
        cancellable.value(), "Migration accepts the new state.");
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled.value());

    QSqlQuery foreignKeyCheck(connection.database());
    ASSERT_TRUE(foreignKeyCheck.exec("PRAGMA foreign_key_check"));
    EXPECT_FALSE(foreignKeyCheck.next());
}

TEST(DatabaseConnectionTest, RejectsAConnectionThatCannotUseWal)
{
    DatabaseConnection connection(std::filesystem::path(":memory:"));

    const auto openResult = connection.open();

    ASSERT_FALSE(openResult);
    EXPECT_EQ(
        openResult.error().code,
        core::ErrorCode::DatabaseOpenFailed);
    EXPECT_FALSE(connection.isOpen());
}

TEST(MigrationRunnerTest, RollsBackAFileWhenOneStatementFails)
{
    test::TemporaryProject project("migration-rollback");
    const auto migrations = project.root() / "migrations";
    std::filesystem::create_directories(migrations);
    {
        std::ofstream migrationFile(migrations / "001_broken.sql");
        ASSERT_TRUE(migrationFile.is_open());
        migrationFile
            << "CREATE TABLE should_be_rolled_back(id INTEGER);\n"
            << "THIS IS NOT SQL;\n";
    }

    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);

    const auto migrationResult = runner.run(migrations);

    ASSERT_FALSE(migrationResult);
    EXPECT_EQ(
        migrationResult.error().code,
        core::ErrorCode::MigrationFailed);

    QSqlQuery tableQuery(connection.database());
    ASSERT_TRUE(tableQuery.prepare(
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type = 'table' AND name = ?"));
    tableQuery.addBindValue(QString("should_be_rolled_back"));
    ASSERT_TRUE(tableQuery.exec());
    ASSERT_TRUE(tableQuery.next());
    EXPECT_EQ(tableQuery.value(0).toInt(), 0);

    QSqlQuery historyQuery(connection.database());
    ASSERT_TRUE(historyQuery.prepare(
        "SELECT COUNT(*) FROM schema_migration WHERE version = ?"));
    historyQuery.addBindValue(QString("001_broken.sql"));
    ASSERT_TRUE(historyQuery.exec());
    ASSERT_TRUE(historyQuery.next());
    EXPECT_EQ(historyQuery.value(0).toInt(), 0);
}

TEST(MigrationRunnerTest, RejectsRepositoryUseFromAnotherThread)
{
    test::TemporaryProject project("thread-owner");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteTaskRepository repository(connection);
    bool rejected = false;
    core::ErrorCode errorCode = core::ErrorCode::InvalidParameter;

    std::thread otherThread([&repository, &rejected, &errorCode]() {
        const auto result = repository.listTasks(10);
        rejected = !result.hasValue();
        if (!result) {
            errorCode = result.error().code;
        }
    });
    otherThread.join();

    EXPECT_TRUE(rejected);
    EXPECT_EQ(errorCode, core::ErrorCode::DatabaseQueryFailed);
}

TEST(MigrationRunnerTest, StoresTheReservedScriptHistoryTable)
{
    test::TemporaryProject project("script-history");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto saved = repository.addScriptHistory(
        "signal = load(1)",
        "FAILED",
        "Parser is introduced in stage 6");
    ASSERT_TRUE(saved);

    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().id, saved.value());
    EXPECT_EQ(history.value().front().status, "FAILED");
    EXPECT_FALSE(history.value().front().createdUtc.empty());
}

TEST(DatabaseExecutorTest, ProxiesEveryRepositoryOnItsDatabaseThread)
{
    test::TemporaryProject project("database-executor");
    DatabaseExecutor executor(
        project.databasePath(), test::migrationsDirectory());

    const auto beforeStart = executor.listTasks(10);
    ASSERT_FALSE(beforeStart);
    EXPECT_EQ(beforeStart.error().code, core::ErrorCode::DatabaseOpenFailed);

    ASSERT_TRUE(executor.start());
    ASSERT_TRUE(executor.start());
    EXPECT_TRUE(executor.isRunning());

    auto signal = core::Signal::create(
        {0.0, 1.0, 0.0, -1.0},
        1'000.0,
        core::SignalMetadata{"Executor signal", "V", "unit test"});
    ASSERT_TRUE(signal);
    const auto storedSignal = executor.saveSignal(
        repository::SignalRegistration{
            repository::SignalSourceType::Generated,
            "database-executor-signal",
            {},
            "{}",
            std::nullopt,
            std::move(signal).value()});
    ASSERT_TRUE(storedSignal);

    const repository::AnalysisTaskStart taskStart{
        storedSignal.value().recordId,
        "Executor signal",
        "executor-test",
        "1.0.0",
        "{}",
        storedSignal.value().sampleHash};
    const auto task = executor.createTask(taskStart);
    ASSERT_TRUE(task);
    const auto started = executor.startTask(task.value());
    ASSERT_TRUE(started);
    ASSERT_TRUE(started.value());
    ASSERT_TRUE(executor.completeTask(
        task.value(), {{"rms", 0.5, "V"}}));

    const auto script = executor.addScriptHistory(
        "signal = load(1)", "SUCCEEDED", "");
    ASSERT_TRUE(script);

    constexpr int callerCount = 12;
    std::atomic<int> successfulCalls{0};
    std::vector<std::thread> callers;
    callers.reserve(callerCount);
    for (int index = 0; index < callerCount; ++index) {
        callers.emplace_back([&executor, &successfulCalls, index]() {
            const auto result = executor.addScriptHistory(
                "concurrent script " + std::to_string(index),
                "SUCCEEDED",
                "");
            if (result) {
                ++successfulCalls;
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    EXPECT_EQ(successfulCalls.load(), callerCount);

    const auto storedSignals = executor.listSignals();
    ASSERT_TRUE(storedSignals);
    EXPECT_EQ(storedSignals.value().size(), 1U);
    const auto history = executor.listTasks(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(
        history.value().front().status,
        repository::AnalysisTaskStatus::Succeeded);
    const auto scripts = executor.listScriptHistory(100);
    ASSERT_TRUE(scripts);
    EXPECT_EQ(scripts.value().size(), callerCount + 1U);

    ASSERT_TRUE(executor.shutdown());
    ASSERT_TRUE(executor.shutdown());
    EXPECT_FALSE(executor.isRunning());
    const auto afterShutdown = executor.listTasks(10);
    ASSERT_FALSE(afterShutdown);
    EXPECT_EQ(
        afterShutdown.error().code,
        core::ErrorCode::DatabaseOpenFailed);

    DatabaseConnection verification(project.databasePath());
    ASSERT_TRUE(verification.open());
    QSqlQuery count(verification.database());
    ASSERT_TRUE(count.exec("SELECT COUNT(*) FROM script_history"));
    ASSERT_TRUE(count.next());
    EXPECT_EQ(count.value(0).toInt(), callerCount + 1);
}

TEST(DatabaseExecutorTest, CancelsAndDrainsALargeSignalLookupPromptly)
{
    using namespace std::chrono_literals;

    test::TemporaryProject project("database-executor-cancel");
    DatabaseExecutor executor(
        project.databasePath(), test::migrationsDirectory());
    ASSERT_TRUE(executor.start());

    constexpr std::size_t sampleCount = std::size_t{1} << 22U;
    auto signal = core::Signal::create(
        std::vector<double>(sampleCount, 0.25),
        48'000.0,
        core::SignalMetadata{"Cancellable lookup", "V", "unit test"});
    ASSERT_TRUE(signal);
    const auto stored = executor.saveSignal(
        repository::SignalRegistration{
            repository::SignalSourceType::Generated,
            "database-executor-cancellable-signal",
            {},
            "{}",
            std::nullopt,
            std::move(signal).value()});
    ASSERT_TRUE(stored) << stored.error().message;

    std::stop_source stopSource;
    std::promise<void> callIssuedPromise;
    auto callIssued = callIssuedPromise.get_future();
    std::promise<core::Result<std::optional<repository::StoredSignal>>>
        resultPromise;
    auto completion = resultPromise.get_future();
    const std::int64_t recordId = stored.value().recordId;
    std::jthread caller([&]() {
        callIssuedPromise.set_value();
        resultPromise.set_value(executor.findByRecordId(
            recordId,
            stopSource.get_token()));
    });

    ASSERT_EQ(callIssued.wait_for(1s), std::future_status::ready);
    // Give the dedicated worker time to enter the large BLOB path. The
    // executor now reports completion only after that worker has observed
    // cancellation and stopped, so the following shutdown also verifies
    // that no hidden database work remains after the caller returns.
    std::this_thread::sleep_for(10ms);
    const auto requestedAt = std::chrono::steady_clock::now();
    stopSource.request_stop();
    ASSERT_EQ(completion.wait_for(200ms), std::future_status::ready);
    const auto completedAt = std::chrono::steady_clock::now();

    const auto result = completion.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::Cancelled);
    EXPECT_LT(completedAt - requestedAt, 200ms);

    const auto shutdownStartedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(executor.shutdown());
    const auto shutdownCompletedAt = std::chrono::steady_clock::now();
    EXPECT_LT(shutdownCompletedAt - shutdownStartedAt, 200ms);
}

}  // namespace
}  // namespace signallab::database
