#include <QSqlQuery>
#include <QVariant>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include "signallab/core/error.hpp"
#include "signallab/database/database_connection.hpp"
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
    EXPECT_EQ(firstRun.value(), 1U);
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

}  // namespace
}  // namespace signallab::database
