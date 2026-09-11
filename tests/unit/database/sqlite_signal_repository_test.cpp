#include <QSqlQuery>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/signal.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "test_database_helpers.hpp"

namespace signallab::database {
namespace {

core::Signal makeSignal(
    std::string name,
    double offset,
    double sampleRate = 1'000.0)
{
    auto result = core::Signal::create(
        {offset, offset + 1.0, offset - 1.0, offset},
        sampleRate,
        core::SignalMetadata{std::move(name), "V", "database test"});
    return std::move(result).value();
}

repository::SignalRegistration makeImportedRegistration(
    char hashCharacter,
    std::string sourceKey,
    std::string name,
    double offset,
    double sampleRate = 1'000.0)
{
    const std::string fileHash(64, hashCharacter);
    return repository::SignalRegistration{
        repository::SignalSourceType::Csv,
        std::move(sourceKey),
        std::filesystem::path("C:/input/example.csv"),
        R"({"delimiter":"comma","value_column":1})",
        repository::FileVersionRegistration{
            fileHash,
            std::filesystem::path("data/imports") / (fileHash + ".csv"),
            "example.csv",
            123U,
        },
        makeSignal(std::move(name), offset, sampleRate),
    };
}

std::int64_t tableCount(DatabaseConnection& connection, const char* tableName)
{
    QSqlQuery query(connection.database());
    const QString sql = QString("SELECT COUNT(*) FROM %1").arg(tableName);
    if (!query.exec(sql) || !query.next()) {
        return -1;
    }
    return query.value(0).toLongLong();
}

TEST(SqliteSignalRepositoryTest, RestoresSamplesMetadataAndTraceHashes)
{
    test::TemporaryProject project("signal-restore");
    std::int64_t savedId = 0;
    std::string savedSampleHash;

    {
        DatabaseConnection connection(project.databasePath());
        ASSERT_TRUE(connection.open());
        MigrationRunner runner(connection);
        ASSERT_TRUE(runner.run(test::migrationsDirectory()));
        SqliteSignalRepository repository(connection);

        auto saved = repository.saveSignal(makeImportedRegistration(
            'a', "file:a", "Stored CSV", 2.0));
        ASSERT_TRUE(saved);
        savedId = saved.value().recordId;
        savedSampleHash = saved.value().sampleHash;
        EXPECT_EQ(savedSampleHash.size(), 64U);
    }

    DatabaseConnection restartedConnection(project.databasePath());
    ASSERT_TRUE(restartedConnection.open());
    MigrationRunner restartedRunner(restartedConnection);
    ASSERT_TRUE(restartedRunner.run(test::migrationsDirectory()));
    SqliteSignalRepository restartedRepository(restartedConnection);

    const auto storedSignals = restartedRepository.listSignals();
    ASSERT_TRUE(storedSignals);
    ASSERT_EQ(storedSignals.value().size(), 1U);
    const auto& restored = storedSignals.value().front();
    EXPECT_EQ(restored.recordId, savedId);
    EXPECT_EQ(restored.sampleHash, savedSampleHash);
    EXPECT_EQ(restored.fileHash, std::string(64, 'a'));
    EXPECT_EQ(restored.signal.metadata().name, "Stored CSV");
    ASSERT_EQ(restored.signal.sampleCount(), 4U);
    EXPECT_DOUBLE_EQ(restored.signal.samples()[0], 2.0);
    EXPECT_DOUBLE_EQ(restored.signal.samples()[2], 1.0);
}

TEST(SqliteSignalRepositoryTest, RollsBackEveryImportTableOnRegistrationFailure)
{
    test::TemporaryProject project("signal-rollback");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    ASSERT_TRUE(repository.saveSignal(makeImportedRegistration(
        'a', "duplicate-source-key", "First", 0.0)));
    const auto duplicate = repository.saveSignal(makeImportedRegistration(
        'b', "duplicate-source-key", "Second", 10.0));

    EXPECT_FALSE(duplicate);
    EXPECT_EQ(tableCount(connection, "file_version"), 1);
    EXPECT_EQ(tableCount(connection, "signal_source"), 1);
    EXPECT_EQ(tableCount(connection, "signal_record"), 1);
    EXPECT_EQ(tableCount(connection, "operation_log"), 1);

    const auto missingHash = repository.findByFileHash(std::string(64, 'b'));
    ASSERT_TRUE(missingHash);
    EXPECT_FALSE(missingHash.value().has_value());
}

TEST(SqliteSignalRepositoryTest, InputHashIncludesTheSampleRate)
{
    test::TemporaryProject project("signal-input-hash");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    const auto first = repository.saveSignal(makeImportedRegistration(
        'a', "rate-1000", "First rate", 0.0, 1'000.0));
    const auto second = repository.saveSignal(makeImportedRegistration(
        'b', "rate-2000", "Second rate", 0.0, 2'000.0));

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value().sampleHash, second.value().sampleHash);
}

TEST(SqliteSignalRepositoryTest, RejectsAnUppercaseFileHash)
{
    test::TemporaryProject project("signal-uppercase-hash");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    const auto result = repository.saveSignal(makeImportedRegistration(
        'A', "uppercase-file-hash", "Uppercase hash", 0.0));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(tableCount(connection, "file_version"), 0);
    EXPECT_EQ(tableCount(connection, "signal_source"), 0);
    EXPECT_EQ(tableCount(connection, "signal_record"), 0);
}

TEST(SqliteSignalRepositoryTest, RejectsInvalidSourceAndFileCombinations)
{
    test::TemporaryProject project("signal-source-invariants");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    auto importedWithoutFile = makeImportedRegistration(
        'a', "missing-file", "Missing file", 0.0);
    importedWithoutFile.fileVersion.reset();
    const auto missingFileResult = repository.saveSignal(
        std::move(importedWithoutFile));

    auto generatedWithFile = makeImportedRegistration(
        'b', "generated-with-file", "Generated", 0.0);
    generatedWithFile.sourceType =
        signallab::repository::SignalSourceType::Generated;
    generatedWithFile.sourcePath.clear();
    const auto generatedFileResult = repository.saveSignal(
        std::move(generatedWithFile));

    ASSERT_FALSE(missingFileResult);
    EXPECT_EQ(
        missingFileResult.error().code,
        core::ErrorCode::InvalidParameter);
    ASSERT_FALSE(generatedFileResult);
    EXPECT_EQ(
        generatedFileResult.error().code,
        core::ErrorCode::InvalidParameter);
    EXPECT_EQ(tableCount(connection, "file_version"), 0);
    EXPECT_EQ(tableCount(connection, "signal_source"), 0);
    EXPECT_EQ(tableCount(connection, "signal_record"), 0);
}

}  // namespace
}  // namespace signallab::database
