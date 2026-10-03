#include <QSqlQuery>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/signal.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "signallab/repository/task_repository.hpp"
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

repository::SignalRegistration makeGeneratedRegistration(
    std::string sourceKey,
    std::string name,
    double offset)
{
    return repository::SignalRegistration{
        repository::SignalSourceType::Generated,
        std::move(sourceKey),
        {},
        "{}",
        std::nullopt,
        makeSignal(std::move(name), offset),
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

TEST(SqliteSignalRepositoryTest, RejectsInvalidAndIgnoresMissingDeletion)
{
    test::TemporaryProject project("signal-delete-invalid");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    const auto invalid = repository.removeSignal(0);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, core::ErrorCode::InvalidParameter);

    const auto missing = repository.removeSignal(42);
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing.value().removed);
    EXPECT_FALSE(
        missing.value().unreferencedStoredRelativePath.has_value());
    EXPECT_EQ(tableCount(connection, "operation_log"), 0);
}

TEST(
    SqliteSignalRepositoryTest,
    DeletesGeneratedSignalButPreservesAnalysisHistoryAndFeatures)
{
    test::TemporaryProject project("signal-delete-generated");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository signalRepository(connection);

    const auto signal = signalRepository.saveSignal(
        makeGeneratedRegistration(
            "generated:delete", "Generated deletion", 3.0));
    ASSERT_TRUE(signal);

    SqliteTaskRepository taskRepository(connection);
    const auto task = taskRepository.createTask(
        repository::AnalysisTaskStart{
            signal.value().recordId,
            signal.value().signal.metadata().name,
            "synchronous-analysis",
            "analysis-service/1.0.0",
            "{}",
            signal.value().sampleHash,
        });
    ASSERT_TRUE(task);
    const auto started = taskRepository.startTask(task.value());
    ASSERT_TRUE(started);
    ASSERT_TRUE(started.value());
    const auto completed = taskRepository.completeTask(
        task.value(), {{"rms", 2.5, "V"}});
    ASSERT_TRUE(completed);

    const auto removed = signalRepository.removeSignal(
        signal.value().recordId);

    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_TRUE(removed.value().removed);
    EXPECT_FALSE(
        removed.value().unreferencedStoredRelativePath.has_value());
    EXPECT_EQ(tableCount(connection, "signal_record"), 0);
    EXPECT_EQ(tableCount(connection, "signal_source"), 0);

    const auto history = taskRepository.listTasks(10);
    ASSERT_TRUE(history);
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(history.value().front().id, task.value());
    EXPECT_FALSE(history.value().front().signalRecordId.has_value());
    EXPECT_EQ(history.value().front().signalName, "Generated deletion");
    EXPECT_EQ(
        history.value().front().status,
        repository::AnalysisTaskStatus::Succeeded);

    const auto features = taskRepository.listFeatureResults(task.value());
    ASSERT_TRUE(features);
    ASSERT_EQ(features.value().size(), 1U);
    EXPECT_EQ(features.value().front().name, "rms");
    ASSERT_TRUE(features.value().front().numericValue.has_value());
    EXPECT_DOUBLE_EQ(*features.value().front().numericValue, 2.5);

    QSqlQuery logQuery(connection.database());
    ASSERT_TRUE(logQuery.prepare(
        "SELECT entity_type, entity_id, detail FROM operation_log "
        "WHERE operation = 'DELETE_SIGNAL'"));
    ASSERT_TRUE(logQuery.exec());
    ASSERT_TRUE(logQuery.next());
    EXPECT_EQ(logQuery.value(0).toString(), "signal_record");
    EXPECT_EQ(logQuery.value(1).toLongLong(), signal.value().recordId);
    EXPECT_EQ(logQuery.value(2).toString(), "database rows removed");
    EXPECT_FALSE(logQuery.next());
}

TEST(
    SqliteSignalRepositoryTest,
    ReturnsArchiveCleanupOnlyWhenTheLastImportedSignalIsDeleted)
{
    test::TemporaryProject project("signal-delete-shared-file");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    const auto first = repository.saveSignal(makeImportedRegistration(
        'a', "shared-file:first", "First channel", 1.0));
    const auto second = repository.saveSignal(makeImportedRegistration(
        'a', "shared-file:second", "Second channel", 2.0));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_EQ(first.value().storedRelativePath,
              second.value().storedRelativePath);
    EXPECT_EQ(tableCount(connection, "file_version"), 1);

    const auto firstRemoval = repository.removeSignal(
        first.value().recordId);
    ASSERT_TRUE(firstRemoval) << firstRemoval.error().message;
    EXPECT_TRUE(firstRemoval.value().removed);
    EXPECT_FALSE(
        firstRemoval.value().unreferencedStoredRelativePath.has_value());
    EXPECT_EQ(tableCount(connection, "file_version"), 1);
    EXPECT_EQ(tableCount(connection, "signal_source"), 1);
    EXPECT_EQ(tableCount(connection, "signal_record"), 1);

    const auto secondRemoval = repository.removeSignal(
        second.value().recordId);
    ASSERT_TRUE(secondRemoval) << secondRemoval.error().message;
    EXPECT_TRUE(secondRemoval.value().removed);
    ASSERT_TRUE(
        secondRemoval.value().unreferencedStoredRelativePath.has_value());
    EXPECT_EQ(
        *secondRemoval.value().unreferencedStoredRelativePath,
        first.value().storedRelativePath);
    EXPECT_EQ(tableCount(connection, "file_version"), 0);
    EXPECT_EQ(tableCount(connection, "signal_source"), 0);
    EXPECT_EQ(tableCount(connection, "signal_record"), 0);

    QSqlQuery logQuery(connection.database());
    ASSERT_TRUE(logQuery.prepare(
        "SELECT entity_id, detail FROM operation_log "
        "WHERE operation = 'DELETE_SIGNAL' ORDER BY id"));
    ASSERT_TRUE(logQuery.exec());
    ASSERT_TRUE(logQuery.next());
    EXPECT_EQ(logQuery.value(0).toLongLong(), first.value().recordId);
    EXPECT_EQ(
        logQuery.value(1).toString(),
        "database rows removed; imported archive remains shared");
    ASSERT_TRUE(logQuery.next());
    EXPECT_EQ(logQuery.value(0).toLongLong(), second.value().recordId);
    EXPECT_TRUE(logQuery.value(1).toString().startsWith(
        "database rows removed; unreferenced archive cleanup required: "));
    EXPECT_TRUE(logQuery.value(1).toString().endsWith(
        QString::fromStdWString(
            first.value().storedRelativePath.filename().wstring())));
    EXPECT_FALSE(logQuery.next());
}

TEST(SqliteSignalRepositoryTest, RollsBackDeletionWhenAuditLoggingFails)
{
    test::TemporaryProject project("signal-delete-rollback");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    const auto signal = repository.saveSignal(makeImportedRegistration(
        'b', "delete-rollback", "Rollback deletion", 4.0));
    ASSERT_TRUE(signal);

    QSqlQuery dropLog(connection.database());
    ASSERT_TRUE(dropLog.exec("DROP TABLE operation_log"));

    const auto removed = repository.removeSignal(signal.value().recordId);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, core::ErrorCode::DatabaseQueryFailed);

    const auto storedSignals = repository.listSignals();
    ASSERT_TRUE(storedSignals);
    ASSERT_EQ(storedSignals.value().size(), 1U);
    EXPECT_EQ(
        storedSignals.value().front().recordId,
        signal.value().recordId);
    EXPECT_EQ(tableCount(connection, "file_version"), 1);
    EXPECT_EQ(tableCount(connection, "signal_source"), 1);
    EXPECT_EQ(tableCount(connection, "signal_record"), 1);
}

TEST(SqliteSignalRepositoryTest,
     SupportsWavLookupsAndRejectsCorruptStoredSignalRows)
{
    test::TemporaryProject project("signal-wav-query-integrity");
    DatabaseConnection connection(project.databasePath());
    ASSERT_TRUE(connection.open());
    MigrationRunner runner(connection);
    ASSERT_TRUE(runner.run(test::migrationsDirectory()));
    SqliteSignalRepository repository(connection);

    auto registration = makeImportedRegistration(
        'f', "wav:lookup", "Stored WAV", 5.0, 48'000.0);
    registration.sourceType = repository::SignalSourceType::Wav;
    registration.sourcePath = std::filesystem::path("C:/input/example.wav");
    registration.fileVersion->originalName = "example.wav";
    registration.fileVersion->storedRelativePath =
        std::filesystem::path("data/imports")
        / (registration.fileVersion->contentHash + ".wav");
    const auto saved = repository.saveSignal(std::move(registration));
    ASSERT_TRUE(saved) << saved.error().message;

    const auto bySource = repository.findBySourceKey("wav:lookup");
    const auto byRecord = repository.findByRecordId(saved.value().recordId);
    const auto byFile = repository.findByFileHash(std::string(64, 'f'));
    ASSERT_TRUE(bySource);
    ASSERT_TRUE(byRecord);
    ASSERT_TRUE(byFile);
    ASSERT_TRUE(bySource.value().has_value());
    ASSERT_TRUE(byRecord.value().has_value());
    ASSERT_TRUE(byFile.value().has_value());
    EXPECT_EQ(
        byRecord.value()->sourceType,
        repository::SignalSourceType::Wav);
    EXPECT_EQ(byRecord.value()->signal.sampleCount(), 4U);
    EXPECT_EQ(byFile.value()->recordId, saved.value().recordId);

    const auto missingSource = repository.findBySourceKey("wav:missing");
    const auto missingFile = repository.findByFileHash(std::string(64, 'e'));
    ASSERT_TRUE(missingSource);
    ASSERT_TRUE(missingFile);
    EXPECT_FALSE(missingSource.value().has_value());
    EXPECT_FALSE(missingFile.value().has_value());

    const auto invalidRecord = repository.findByRecordId(0);
    ASSERT_FALSE(invalidRecord);
    EXPECT_EQ(
        invalidRecord.error().code,
        core::ErrorCode::InvalidParameter);
    std::stop_source stopSource;
    stopSource.request_stop();
    const auto cancelledRecord = repository.findByRecordId(
        saved.value().recordId,
        stopSource.get_token());
    ASSERT_FALSE(cancelledRecord);
    EXPECT_EQ(cancelledRecord.error().code, core::ErrorCode::Cancelled);

    QSqlQuery mutation(connection.database());
    ASSERT_TRUE(mutation.exec("PRAGMA ignore_check_constraints = ON"));
    ASSERT_TRUE(mutation.prepare(
        "UPDATE signal_source SET source_type = 'UNKNOWN' WHERE id = ?"));
    mutation.addBindValue(saved.value().sourceId);
    ASSERT_TRUE(mutation.exec());
    auto corrupted = repository.findByRecordId(saved.value().recordId);
    ASSERT_FALSE(corrupted);
    EXPECT_EQ(corrupted.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE signal_source SET source_type = 'WAV' WHERE id = ?"));
    mutation.addBindValue(saved.value().sourceId);
    ASSERT_TRUE(mutation.exec());
    ASSERT_TRUE(mutation.prepare(
        "UPDATE signal_record SET sample_hash = ? WHERE id = ?"));
    mutation.addBindValue(QString(64, QChar('b')));
    mutation.addBindValue(saved.value().recordId);
    ASSERT_TRUE(mutation.exec());
    corrupted = repository.findByRecordId(saved.value().recordId);
    ASSERT_FALSE(corrupted);
    EXPECT_EQ(corrupted.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE signal_record SET sample_hash = ?, sample_count = 5 "
        "WHERE id = ?"));
    mutation.addBindValue(QString::fromStdString(saved.value().sampleHash));
    mutation.addBindValue(saved.value().recordId);
    ASSERT_TRUE(mutation.exec());
    corrupted = repository.findByRecordId(saved.value().recordId);
    ASSERT_FALSE(corrupted);
    EXPECT_EQ(corrupted.error().code, core::ErrorCode::DatabaseQueryFailed);

    ASSERT_TRUE(mutation.prepare(
        "UPDATE signal_record SET sample_count = 4 WHERE id = ?"));
    mutation.addBindValue(saved.value().recordId);
    ASSERT_TRUE(mutation.exec());
    const auto restored = repository.findByRecordId(saved.value().recordId);
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restored.value().has_value());
    EXPECT_EQ(restored.value()->signal.metadata().name, "Stored WAV");
}

}  // namespace
}  // namespace signallab::database
