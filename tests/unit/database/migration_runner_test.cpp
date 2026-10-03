#include <QSqlError>
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
    EXPECT_EQ(firstRun.value(), 3U);
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

TEST(MigrationRunnerTest, StageSixMigrationPreservesLegacyScriptHistory)
{
    test::TemporaryProject project("migration-stage-six");
    const auto migrations = project.root() / "migrations";
    std::filesystem::create_directories(migrations);
    std::filesystem::copy_file(
        test::migrationsDirectory() / "001_init.sql",
        migrations / "001_init.sql");
    std::filesystem::copy_file(
        test::migrationsDirectory() / "002_task_scheduler.sql",
        migrations / "002_task_scheduler.sql");

    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    const auto initialMigration = runner.run(migrations);
    ASSERT_TRUE(initialMigration);
    ASSERT_EQ(initialMigration.value(), 2U);

    QSqlQuery legacyInsert(connection.database());
    ASSERT_TRUE(legacyInsert.prepare(
        "INSERT INTO script_history("
        "script_text, status, created_utc, finished_utc, error_message"
        ") VALUES (?, ?, ?, ?, ?)"));
    legacyInsert.addBindValue(QStringLiteral("legacy source\n"));
    legacyInsert.addBindValue(QStringLiteral("CUSTOM_LEGACY_STATUS"));
    legacyInsert.addBindValue(QStringLiteral("2026-01-02T03:04:05.006Z"));
    legacyInsert.addBindValue(QStringLiteral("2026-01-02T03:04:06.007Z"));
    legacyInsert.addBindValue(QStringLiteral("legacy message"));
    ASSERT_TRUE(legacyInsert.exec());
    const auto legacyId = legacyInsert.lastInsertId().toLongLong();

    std::filesystem::copy_file(
        test::migrationsDirectory() / "003_signal_script.sql",
        migrations / "003_signal_script.sql");
    const auto stageSixMigration = runner.run(migrations);
    ASSERT_TRUE(stageSixMigration);
    EXPECT_EQ(stageSixMigration.value(), 1U);
    const auto repeatedMigration = runner.run(migrations);
    ASSERT_TRUE(repeatedMigration);
    EXPECT_EQ(repeatedMigration.value(), 0U);

    SqliteScriptRepository repository(connection);
    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    const auto& legacy = history.value().front();
    EXPECT_EQ(legacy.id, legacyId);
    EXPECT_EQ(legacy.scriptText, "legacy source\n");
    EXPECT_TRUE(legacy.sourceHash.empty());
    EXPECT_EQ(legacy.status, repository::ScriptHistoryStatus::Legacy);
    EXPECT_EQ(legacy.statusText, "CUSTOM_LEGACY_STATUS");
    EXPECT_EQ(legacy.createdUtc, "2026-01-02T03:04:05.006Z");
    EXPECT_EQ(legacy.finishedUtc, "2026-01-02T03:04:06.007Z");
    EXPECT_FALSE(legacy.errorLocation.has_value());
    EXPECT_EQ(legacy.errorMessage, "legacy message");

    const auto newHistory = repository.createScriptHistory("new source");
    ASSERT_TRUE(newHistory);
    EXPECT_GT(newHistory.value(), legacyId);

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

TEST(SqliteScriptRepositoryTest, StoresHashAndTerminalDiagnostic)
{
    test::TemporaryProject project("script-history");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto created = repository.createScriptHistory("signal = load(1)");
    ASSERT_TRUE(created);

    const auto running = repository.listScriptHistory(10);
    ASSERT_TRUE(running);
    ASSERT_EQ(running.value().size(), 1U);
    EXPECT_EQ(
        running.value().front().status,
        repository::ScriptHistoryStatus::Running);
    EXPECT_TRUE(running.value().front().finishedUtc.empty());
    EXPECT_EQ(
        running.value().front().sourceHash,
        "4ac4e3e179447c117e43b0e8f09c78bc1c8c7e4767aa4565b40830bd170c6e50");

    const auto invalidLocation = repository.finishScriptHistory(
        created.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Failed,
            repository::ScriptErrorLocation{0, 4},
            "Invalid location"});
    ASSERT_FALSE(invalidLocation);
    EXPECT_EQ(
        invalidLocation.error().code,
        core::ErrorCode::InvalidParameter);

    const auto finished = repository.finishScriptHistory(
        created.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Failed,
            repository::ScriptErrorLocation{2, 9},
            "Expected an expression."});
    ASSERT_TRUE(finished);
    EXPECT_TRUE(finished.value());

    const auto repeated = repository.finishScriptHistory(
        created.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Cancelled,
            std::nullopt,
            "Too late."});
    ASSERT_TRUE(repeated);
    EXPECT_FALSE(repeated.value());

    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    const auto& record = history.value().front();
    EXPECT_EQ(record.id, created.value());
    EXPECT_EQ(record.status, repository::ScriptHistoryStatus::Failed);
    EXPECT_EQ(record.statusText, "FAILED");
    ASSERT_TRUE(record.errorLocation.has_value());
    EXPECT_EQ(record.errorLocation->line, 2);
    EXPECT_EQ(record.errorLocation->column, 9);
    EXPECT_EQ(record.errorMessage, "Expected an expression.");
    EXPECT_FALSE(record.createdUtc.empty());
    EXPECT_FALSE(record.finishedUtc.empty());
}

TEST(SqliteScriptRepositoryTest, PreservesExactUtf8SourceBytes)
{
    test::TemporaryProject project("script-history-utf8");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    // Construct the non-ASCII bytes explicitly so this test remains
    // independent of the compiler's source-file execution encoding.
    const std::string source =
        "load \""
        "\xE4\xB8\xAD\xE6\x96\x87"
        ".csv\" as raw;\r\n# exact\r\n";
    SqliteScriptRepository repository(connection);
    const auto created = repository.createScriptHistory(source);
    ASSERT_TRUE(created) << created.error().message;

    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().scriptText, source);
    EXPECT_EQ(
        history.value().front().sourceHash,
        "235becb2266c1867b4a20b955a68601e35c65ea152eca5e0ca6d592a57d544c9");

    const std::string invalidUtf8{"\xC0\xAF", 2};
    const auto rejected = repository.createScriptHistory(invalidUtf8);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, core::ErrorCode::InvalidParameter);
}

TEST(SqliteScriptRepositoryTest, DoesNotMutateLegacyRunningHistory)
{
    test::TemporaryProject project("script-legacy-running");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    QSqlQuery insert(connection.database());
    ASSERT_TRUE(insert.prepare(
        "INSERT INTO script_history("
        "script_text, status, created_utc, finished_utc, error_message"
        ") VALUES (?, 'RUNNING', ?, ?, '')"));
    insert.addBindValue(QStringLiteral("legacy running label"));
    insert.addBindValue(QStringLiteral("2026-01-02T03:04:05.006Z"));
    insert.addBindValue(QStringLiteral("2026-01-02T03:04:06.007Z"));
    ASSERT_TRUE(insert.exec()) << insert.lastError().text().toStdString();
    const auto legacyId = insert.lastInsertId().toLongLong();

    SqliteScriptRepository repository(connection);
    const auto finish = repository.finishScriptHistory(
        legacyId,
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Failed,
            std::nullopt,
            "Must remain unchanged."});
    ASSERT_TRUE(finish) << finish.error().message;
    EXPECT_FALSE(finish.value());

    const auto interrupted = repository.interruptRunningScripts();
    ASSERT_TRUE(interrupted) << interrupted.error().message;
    EXPECT_EQ(interrupted.value(), 0U);

    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().id, legacyId);
    EXPECT_TRUE(history.value().front().sourceHash.empty());
    EXPECT_EQ(
        history.value().front().status,
        repository::ScriptHistoryStatus::Running);
    EXPECT_EQ(history.value().front().statusText, "RUNNING");
    EXPECT_EQ(
        history.value().front().finishedUtc,
        "2026-01-02T03:04:06.007Z");
    EXPECT_TRUE(history.value().front().errorMessage.empty());
}

TEST(SqliteScriptRepositoryTest, InterruptsOnlyRunningScripts)
{
    test::TemporaryProject project("script-interruption");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto completedId = repository.createScriptHistory("completed");
    const auto runningId = repository.createScriptHistory("running");
    ASSERT_TRUE(completedId);
    ASSERT_TRUE(runningId);
    const auto completed = repository.finishScriptHistory(
        completedId.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Succeeded,
            std::nullopt,
            ""});
    ASSERT_TRUE(completed);
    ASSERT_TRUE(completed.value());

    const auto interrupted = repository.interruptRunningScripts();
    ASSERT_TRUE(interrupted);
    EXPECT_EQ(interrupted.value(), 1U);
    const auto repeated = repository.interruptRunningScripts();
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.value(), 0U);

    const auto history = repository.listScriptHistory(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 2U);
    EXPECT_EQ(history.value()[0].id, runningId.value());
    EXPECT_EQ(
        history.value()[0].status,
        repository::ScriptHistoryStatus::Interrupted);
    EXPECT_FALSE(history.value()[0].errorMessage.empty());
    EXPECT_EQ(history.value()[1].id, completedId.value());
    EXPECT_EQ(
        history.value()[1].status,
        repository::ScriptHistoryStatus::Succeeded);
}

TEST(SqliteScriptRepositoryTest, MigrationConstraintsRejectInvalidNewRows)
{
    test::TemporaryProject project("script-constraints");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto created = repository.createScriptHistory("constrained");
    ASSERT_TRUE(created);

    QSqlQuery invalidHash(connection.database());
    invalidHash.prepare(
        "UPDATE script_history SET source_hash = ? WHERE id = ?");
    invalidHash.addBindValue(QString(64, QChar('A')));
    invalidHash.addBindValue(created.value());
    EXPECT_FALSE(invalidHash.exec());

    QSqlQuery invalidStatus(connection.database());
    invalidStatus.prepare(
        "UPDATE script_history SET status = 'UNKNOWN' WHERE id = ?");
    invalidStatus.addBindValue(created.value());
    EXPECT_FALSE(invalidStatus.exec());

    QSqlQuery incompleteLocation(connection.database());
    incompleteLocation.prepare(
        "UPDATE script_history SET error_line = 1, error_column = NULL "
        "WHERE id = ?");
    incompleteLocation.addBindValue(created.value());
    EXPECT_FALSE(incompleteLocation.exec());

    QSqlQuery successWithError(connection.database());
    successWithError.prepare(
        "UPDATE script_history SET status = 'SUCCEEDED', finished_utc = ?, "
        "error_message = 'not allowed' WHERE id = ?");
    successWithError.addBindValue(QStringLiteral("2026-09-17T00:00:00.000Z"));
    successWithError.addBindValue(created.value());
    EXPECT_FALSE(successWithError.exec());

    QSqlQuery fractionalLocation(connection.database());
    fractionalLocation.prepare(
        "UPDATE script_history SET status = 'FAILED', finished_utc = ?, "
        "error_message = 'invalid location', error_line = 1.5, "
        "error_column = 2 WHERE id = ?");
    fractionalLocation.addBindValue(
        QStringLiteral("2026-09-17T00:00:00.000Z"));
    fractionalLocation.addBindValue(created.value());
    EXPECT_FALSE(fractionalLocation.exec());
}

TEST(SqliteScriptRepositoryTest, DetectsSourceHashMismatch)
{
    test::TemporaryProject project("script-hash-mismatch");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto created = repository.createScriptHistory("original");
    ASSERT_TRUE(created);
    QSqlQuery tamper(connection.database());
    tamper.prepare("UPDATE script_history SET script_text = ? WHERE id = ?");
    tamper.addBindValue(QStringLiteral("tampered"));
    tamper.addBindValue(created.value());
    ASSERT_TRUE(tamper.exec());

    const auto history = repository.listScriptHistory(10);
    ASSERT_FALSE(history);
    EXPECT_EQ(history.error().code, core::ErrorCode::DatabaseQueryFailed);
}

TEST(SqliteScriptRepositoryTest, RollsBackFinishWhenLogCannotBeWritten)
{
    test::TemporaryProject project("script-transaction");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));

    SqliteScriptRepository repository(connection);
    const auto created = repository.createScriptHistory("rolled back");
    ASSERT_TRUE(created);
    QSqlQuery dropLog(connection.database());
    ASSERT_TRUE(dropLog.exec("DROP TABLE operation_log"));

    const auto finished = repository.finishScriptHistory(
        created.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Failed,
            repository::ScriptErrorLocation{3, 7},
            "This update must roll back."});
    ASSERT_FALSE(finished);
    EXPECT_EQ(finished.error().code, core::ErrorCode::DatabaseQueryFailed);

    QSqlQuery state(connection.database());
    state.prepare(
        "SELECT status, finished_utc, error_line, error_column, "
        "error_message FROM script_history WHERE id = ?");
    state.addBindValue(created.value());
    ASSERT_TRUE(state.exec());
    ASSERT_TRUE(state.next());
    EXPECT_EQ(state.value(0).toString(), "RUNNING");
    EXPECT_TRUE(state.value(1).toString().isEmpty());
    EXPECT_TRUE(state.value(2).isNull());
    EXPECT_TRUE(state.value(3).isNull());
    EXPECT_TRUE(state.value(4).toString().isEmpty());
}

TEST(SqliteScriptRepositoryTest,
     ValidatesTerminalRequestsAndPersistsEveryTerminalStatus)
{
    test::TemporaryProject project("script-terminal-boundaries");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteScriptRepository repository(connection);

    const auto invalidId = repository.finishScriptHistory(
        0,
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Succeeded,
            std::nullopt,
            ""});
    ASSERT_FALSE(invalidId);
    EXPECT_EQ(invalidId.error().code, core::ErrorCode::InvalidParameter);

    const auto runningStatus = repository.finishScriptHistory(
        1,
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Running,
            std::nullopt,
            "Still running"});
    ASSERT_FALSE(runningStatus);
    EXPECT_EQ(runningStatus.error().code, core::ErrorCode::InvalidParameter);

    const auto successWithError = repository.finishScriptHistory(
        1,
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Succeeded,
            std::nullopt,
            "Success cannot carry an error"});
    ASSERT_FALSE(successWithError);
    EXPECT_EQ(
        successWithError.error().code,
        core::ErrorCode::InvalidParameter);

    const auto failureWithoutMessage = repository.finishScriptHistory(
        1,
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Failed,
            std::nullopt,
            ""});
    ASSERT_FALSE(failureWithoutMessage);
    EXPECT_EQ(
        failureWithoutMessage.error().code,
        core::ErrorCode::InvalidParameter);

    const auto interruptedId = repository.createScriptHistory("interrupted");
    const auto cancelledId = repository.createScriptHistory("cancelled");
    ASSERT_TRUE(interruptedId);
    ASSERT_TRUE(cancelledId);
    const auto interrupted = repository.finishScriptHistory(
        interruptedId.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Interrupted,
            std::nullopt,
            "Stopped during shutdown."});
    const auto cancelled = repository.finishScriptHistory(
        cancelledId.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Cancelled,
            std::nullopt,
            "Cancelled by the user."});
    ASSERT_TRUE(interrupted);
    EXPECT_TRUE(interrupted.value());
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled.value());

    const auto history = repository.listScriptHistory(1);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().id, cancelledId.value());
    EXPECT_EQ(
        history.value().front().status,
        repository::ScriptHistoryStatus::Cancelled);

    const auto allHistory = repository.listScriptHistory(10);
    ASSERT_TRUE(allHistory);
    ASSERT_EQ(allHistory.value().size(), 2U);
    EXPECT_EQ(
        allHistory.value()[1].status,
        repository::ScriptHistoryStatus::Interrupted);
    EXPECT_EQ(
        allHistory.value()[1].errorMessage,
        "Stopped during shutdown.");
}

TEST(SqliteScriptRepositoryTest,
     RejectsCorruptStatusAndSourceLocationsWithoutHidingLegacyRows)
{
    test::TemporaryProject project("script-corrupt-readback");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteScriptRepository repository(connection);

    const auto created = repository.createScriptHistory("checked history");
    ASSERT_TRUE(created);
    QSqlQuery mutation(connection.database());
    ASSERT_TRUE(mutation.exec("PRAGMA ignore_check_constraints = ON"));

    ASSERT_TRUE(mutation.prepare(
        "UPDATE script_history SET status = 'UNKNOWN' WHERE id = ?"));
    mutation.addBindValue(created.value());
    ASSERT_TRUE(mutation.exec());
    auto history = repository.listScriptHistory(10);
    ASSERT_FALSE(history);
    EXPECT_EQ(history.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE script_history SET status = 'FAILED', finished_utc = ?, "
        "error_message = 'failure', error_line = 1, error_column = NULL "
        "WHERE id = ?"));
    mutation.addBindValue(QStringLiteral("2026-10-03T00:00:00.000Z"));
    mutation.addBindValue(created.value());
    ASSERT_TRUE(mutation.exec());
    history = repository.listScriptHistory(10);
    ASSERT_FALSE(history);
    EXPECT_EQ(history.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE script_history SET error_line = 0, error_column = 2 "
        "WHERE id = ?"));
    mutation.addBindValue(created.value());
    ASSERT_TRUE(mutation.exec());
    history = repository.listScriptHistory(10);
    ASSERT_FALSE(history);
    EXPECT_EQ(history.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE script_history SET status = 'OLD_CUSTOM_STATUS', "
        "source_hash = '', error_line = NULL, error_column = NULL "
        "WHERE id = ?"));
    mutation.addBindValue(created.value());
    ASSERT_TRUE(mutation.exec());
    history = repository.listScriptHistory(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(
        history.value().front().status,
        repository::ScriptHistoryStatus::Legacy);
    EXPECT_EQ(history.value().front().statusText, "OLD_CUSTOM_STATUS");
    EXPECT_FALSE(history.value().front().errorLocation.has_value());
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

    const auto script = executor.createScriptHistory("signal = load(1)");
    ASSERT_TRUE(script);
    const auto scriptFinished = executor.finishScriptHistory(
        script.value(),
        repository::ScriptHistoryFinish{
            repository::ScriptHistoryStatus::Succeeded,
            std::nullopt,
            ""});
    ASSERT_TRUE(scriptFinished);
    ASSERT_TRUE(scriptFinished.value());

    constexpr int callerCount = 12;
    std::atomic<int> successfulCalls{0};
    std::vector<std::thread> callers;
    callers.reserve(callerCount);
    for (int index = 0; index < callerCount; ++index) {
        callers.emplace_back([&executor, &successfulCalls, index]() {
            const auto created = executor.createScriptHistory(
                "concurrent script " + std::to_string(index));
            if (!created) {
                return;
            }
            const auto finished = executor.finishScriptHistory(
                created.value(),
                repository::ScriptHistoryFinish{
                    repository::ScriptHistoryStatus::Succeeded,
                    std::nullopt,
                    ""});
            if (finished && finished.value()) {
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
    const auto removedTask = executor.removeTask(task.value());
    ASSERT_TRUE(removedTask);
    EXPECT_TRUE(removedTask.value());
    const auto historyAfterDelete = executor.listTasks(10);
    ASSERT_TRUE(historyAfterDelete);
    EXPECT_TRUE(historyAfterDelete.value().empty());
    const auto scripts = executor.listScriptHistory(100);
    ASSERT_TRUE(scripts);
    EXPECT_EQ(scripts.value().size(), callerCount + 1U);

    const auto raced = executor.createScriptHistory("terminal race");
    ASSERT_TRUE(raced);
    std::atomic<int> finishWinners{0};
    std::thread firstFinisher([&]() {
        const auto result = executor.finishScriptHistory(
            raced.value(),
            repository::ScriptHistoryFinish{
                repository::ScriptHistoryStatus::Failed,
                repository::ScriptErrorLocation{1, 1},
                "First terminal result."});
        if (result && result.value()) {
            ++finishWinners;
        }
    });
    std::thread secondFinisher([&]() {
        const auto result = executor.finishScriptHistory(
            raced.value(),
            repository::ScriptHistoryFinish{
                repository::ScriptHistoryStatus::Cancelled,
                std::nullopt,
                "Second terminal result."});
        if (result && result.value()) {
            ++finishWinners;
        }
    });
    firstFinisher.join();
    secondFinisher.join();
    EXPECT_EQ(finishWinners.load(), 1);

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
    EXPECT_EQ(count.value(0).toInt(), callerCount + 2);
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
