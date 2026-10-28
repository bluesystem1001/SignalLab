#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlQuery>
#include <QVariant>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/project_backup_source.hpp"
#include "test_database_helpers.hpp"

namespace signallab::database {
namespace {

class ProjectBackupSourceTest : public ::testing::Test {
protected:
    ProjectBackupSourceTest()
        : project_("project-backup"),
          connection_(project_.root() / "data" / "signallab.sqlite3"),
          source_(project_.root())
    {
    }

    void SetUp() override
    {
        ASSERT_TRUE(connection_.open());
        MigrationRunner runner(connection_);
        ASSERT_TRUE(runner.run(test::migrationsDirectory()));
        std::filesystem::create_directories(staging());
    }

    std::filesystem::path staging() const
    {
        return project_.root() / "staging";
    }

    std::string addImportedFile(const QByteArray& bytes)
    {
        const auto hash = QCryptographicHash::hash(
            bytes, QCryptographicHash::Sha256).toHex().toStdString();
        const auto relative = "data/imports/" + hash + ".source";
        std::filesystem::create_directories(project_.root() / "data" / "imports");
        QFile file(QString::fromStdWString((project_.root() / relative).wstring()));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(bytes), bytes.size());
        file.close();
        QSqlQuery query(connection_.database());
        EXPECT_TRUE(query.prepare(
            "INSERT INTO file_version(content_hash, stored_relative_path, "
            "original_name, byte_size, created_utc) VALUES (?, ?, 'source.csv', ?, '2026-10-11')"));
        query.addBindValue(QString::fromStdString(hash));
        query.addBindValue(QString::fromStdString(relative));
        query.addBindValue(bytes.size());
        EXPECT_TRUE(query.exec());
        return relative;
    }

    void changeStoredPath(const std::string& value)
    {
        QSqlQuery query(connection_.database());
        ASSERT_TRUE(query.prepare("UPDATE file_version SET stored_relative_path=?"));
        query.addBindValue(QString::fromStdString(value));
        ASSERT_TRUE(query.exec());
    }

    test::TemporaryProject project_;
    DatabaseConnection connection_;
    ProjectBackupSource source_;
};

TEST_F(ProjectBackupSourceTest, CapturesLatestCommittedWalDataWithoutChangingSource)
{
    QSqlQuery query(connection_.database());
    ASSERT_TRUE(query.exec("PRAGMA wal_autocheckpoint=0"));
    ASSERT_TRUE(query.exec(
        "INSERT INTO operation_log(operation, entity_type, detail, created_utc) "
        "VALUES ('BACKUP_TEST', 'project', 'latest WAL row', '2026-10-11')"));
    query.finish();
    ASSERT_TRUE(std::filesystem::exists(
        project_.root() / "data" / "signallab.sqlite3-wal"));

    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(snapshot.value().size(), 1U);
    EXPECT_EQ(snapshot.value().front().relativePath, "data/signallab.sqlite3");
    EXPECT_TRUE(snapshot.value().front().localPath.is_absolute());
    EXPECT_EQ(snapshot.value().front().sha256.size(), 64U);
    const auto verified = source_.verify(staging(), snapshot.value());
    ASSERT_TRUE(verified) << verified.error().message;

    DatabaseConnection restored(staging() / "data" / "signallab.sqlite3");
    ASSERT_TRUE(restored.open());
    QSqlQuery read(restored.database());
    ASSERT_TRUE(read.exec("SELECT detail FROM operation_log WHERE operation='BACKUP_TEST'"));
    ASSERT_TRUE(read.next());
    EXPECT_EQ(read.value(0).toString(), "latest WAL row");
    ASSERT_TRUE(query.exec("PRAGMA journal_mode"));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toString(), "wal");
}

TEST_F(ProjectBackupSourceTest, CopiesOnlyReferencedImportedFilesWithExactBytes)
{
    QByteArray bytes("time,value\n0,1.25\n1,-2.5\n");
    bytes.append('\0');
    bytes.append("end");
    const auto relative = addImportedFile(bytes);
    QFile extra(QString::fromStdWString(
        (project_.root() / "data" / "imports" / "unreferenced.source").wstring()));
    ASSERT_TRUE(extra.open(QIODevice::WriteOnly));
    ASSERT_EQ(extra.write("unused"), 6);
    extra.close();

    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(snapshot.value().size(), 2U);
    EXPECT_EQ(snapshot.value()[1].relativePath, relative);
    EXPECT_EQ(snapshot.value()[1].byteSize, static_cast<std::uint64_t>(bytes.size()));
    QFile restored(QString::fromStdWString((staging() / relative).wstring()));
    ASSERT_TRUE(restored.open(QIODevice::ReadOnly));
    EXPECT_EQ(restored.readAll(), bytes);
    restored.close();
    const auto verified = source_.verify(staging(), snapshot.value());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_FALSE(std::filesystem::exists(
        staging() / "data" / "imports" / "unreferenced.source"));
}

TEST_F(ProjectBackupSourceTest, PreservesLegacyCsvAndWavImportedSourcePaths)
{
    std::vector<std::string> legacyPaths;
    for (const auto& extension : {std::string(".csv"), std::string(".wav")}) {
        const auto original = addImportedFile(QByteArray::fromStdString(
            "legacy imported content " + extension));
        auto legacy = std::filesystem::path(original);
        legacy.replace_extension(extension);
        const auto relative = legacy.generic_string();
        std::filesystem::rename(project_.root() / original, project_.root() / legacy);
        QSqlQuery query(connection_.database());
        ASSERT_TRUE(query.prepare(
            "UPDATE file_version SET stored_relative_path=? WHERE stored_relative_path=?"));
        query.addBindValue(QString::fromStdString(relative));
        query.addBindValue(QString::fromStdString(original));
        ASSERT_TRUE(query.exec());
        legacyPaths.push_back(relative);
    }

    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(snapshot.value().size(), 3U);
    for (const auto& relative : legacyPaths) {
        const auto found = std::find_if(snapshot.value().begin(), snapshot.value().end(),
            [&relative](const repository::BackupFile& file) {
                return file.relativePath == relative;
            });
        ASSERT_NE(found, snapshot.value().end());
        EXPECT_TRUE(std::filesystem::exists(found->localPath));
        EXPECT_TRUE(std::filesystem::exists(project_.root() / relative));
        QFile original(QString::fromStdWString((project_.root() / relative).wstring()));
        QFile copied(QString::fromStdWString(found->localPath.wstring()));
        ASSERT_TRUE(original.open(QIODevice::ReadOnly));
        ASSERT_TRUE(copied.open(QIODevice::ReadOnly));
        EXPECT_EQ(copied.readAll(), original.readAll());
    }
    const auto verified = source_.verify(staging(), snapshot.value());
    ASSERT_TRUE(verified) << verified.error().message;
}

TEST_F(ProjectBackupSourceTest, FailsSnapshotWhenReferencedImportDisappears)
{
    const auto relative = addImportedFile("0,1\n");
    ASSERT_TRUE(std::filesystem::remove(project_.root() / relative));
    EXPECT_FALSE(source_.snapshot(staging()));
}

TEST_F(ProjectBackupSourceTest, FailsSnapshotWhenReferencedImportHasDifferentHash)
{
    const auto relative = addImportedFile("0,1\n");
    QFile changed(QString::fromStdWString((project_.root() / relative).wstring()));
    ASSERT_TRUE(changed.open(QIODevice::WriteOnly));
    ASSERT_EQ(changed.write("0,2\n"), 4);
    changed.close();
    EXPECT_FALSE(source_.snapshot(staging()));
}

TEST_F(ProjectBackupSourceTest, FailsSnapshotWhenDatabaseImportSizeIsWrong)
{
    addImportedFile("0,1\n");
    QSqlQuery query(connection_.database());
    ASSERT_TRUE(query.exec("UPDATE file_version SET byte_size=999"));
    EXPECT_FALSE(source_.snapshot(staging()));
}

TEST_F(ProjectBackupSourceTest, RejectsTraversalAndAbsoluteImportPaths)
{
    addImportedFile("0,1\n");
    const std::vector<std::string> invalidPaths{
        "../outside.source",
        "data/imports/../../outside.source",
        "C:/outside.source",
        "data\\imports\\" + std::string(64, 'a') + ".source",
        "data/imports/" + std::string(64, 'A') + ".source"
    };
    for (std::size_t index = 0; index < invalidPaths.size(); ++index) {
        changeStoredPath(invalidPaths[index]);
        const auto destination = project_.root() / ("invalid-" + std::to_string(index));
        std::filesystem::create_directory(destination);
        EXPECT_FALSE(source_.snapshot(destination)) << invalidPaths[index];
    }
}

TEST_F(ProjectBackupSourceTest, RejectsSymlinkedImportWhenPlatformAllowsSymlinks)
{
    const auto relative = addImportedFile("0,1\n");
    const auto original = project_.root() / relative;
    const auto moved = project_.root() / "external.source";
    std::filesystem::rename(original, moved);
    std::error_code error;
    std::filesystem::create_symlink(moved, original, error);
    if (error) {
        GTEST_SKIP() << "The platform does not permit creating test symlinks.";
    }
    EXPECT_FALSE(source_.snapshot(staging()));
}

TEST_F(ProjectBackupSourceTest, NeverCreatesAMissingSourceDatabase)
{
    const auto missingRoot = project_.root() / "missing-project";
    std::filesystem::create_directory(missingRoot);
    ProjectBackupSource missing(missingRoot);
    EXPECT_FALSE(missing.snapshot(staging()));
    EXPECT_FALSE(std::filesystem::exists(missingRoot / "data" / "signallab.sqlite3"));
}

TEST_F(ProjectBackupSourceTest, RefusesNonemptyStagingDirectory)
{
    std::filesystem::create_directory(staging() / "existing");
    EXPECT_FALSE(source_.snapshot(staging()));
    EXPECT_TRUE(std::filesystem::is_directory(staging() / "existing"));
}

TEST_F(ProjectBackupSourceTest, CancelsBeforeCreatingSnapshotOrVerifying)
{
    std::stop_source stop;
    stop.request_stop();
    const auto snapshot = source_.snapshot(staging(), stop.get_token());
    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, core::ErrorCode::Cancelled);
    EXPECT_TRUE(std::filesystem::is_empty(staging()));
    const auto verified = source_.verify(staging(), {}, stop.get_token());
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, core::ErrorCode::Cancelled);
}

TEST_F(ProjectBackupSourceTest, RejectsTamperedRestoreEvenWithSameFileSize)
{
    const auto relative = addImportedFile("0,1\n");
    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    QFile changed(QString::fromStdWString((staging() / relative).wstring()));
    ASSERT_TRUE(changed.open(QIODevice::WriteOnly));
    ASSERT_EQ(changed.write("0,2\n"), 4);
    changed.close();
    EXPECT_FALSE(source_.verify(staging(), snapshot.value()));
}

TEST_F(ProjectBackupSourceTest, RejectsUnexpectedFilesAndMissingManifestEntries)
{
    addImportedFile("0,1\n");
    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto incomplete = snapshot.value();
    incomplete.pop_back();
    EXPECT_FALSE(source_.verify(staging(), incomplete));

    QFile extra(QString::fromStdWString((staging() / "extra.txt").wstring()));
    ASSERT_TRUE(extra.open(QIODevice::WriteOnly));
    ASSERT_EQ(extra.write("unexpected"), 10);
    extra.close();
    EXPECT_FALSE(source_.verify(staging(), snapshot.value()));
}

TEST_F(ProjectBackupSourceTest, RejectsImportManifestThatDisagreesWithDatabase)
{
    const auto relative = addImportedFile("0,1\n");
    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto changedManifest = snapshot.value();
    const QByteArray changedBytes("0,2\n");
    QFile changed(QString::fromStdWString((staging() / relative).wstring()));
    ASSERT_TRUE(changed.open(QIODevice::WriteOnly));
    ASSERT_EQ(changed.write(changedBytes), changedBytes.size());
    changed.close();
    changedManifest[1].sha256 = QCryptographicHash::hash(
        changedBytes, QCryptographicHash::Sha256).toHex().toStdString();
    EXPECT_FALSE(source_.verify(staging(), changedManifest));
}

TEST_F(ProjectBackupSourceTest, RejectsManifestPathsOutsideRestoredDirectory)
{
    addImportedFile("0,1\n");
    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto changed = snapshot.value();
    changed[1].relativePath = "../outside.source";
    EXPECT_FALSE(source_.verify(staging(), changed));
    changed = snapshot.value();
    changed[1].localPath = project_.root() / changed[1].relativePath;
    EXPECT_FALSE(source_.verify(staging(), changed));
}

TEST_F(ProjectBackupSourceTest, RejectsMissingDatabaseAndDuplicateManifestEntries)
{
    const auto snapshot = source_.snapshot(staging());
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto duplicated = snapshot.value();
    duplicated.push_back(duplicated.front());
    EXPECT_FALSE(source_.verify(staging(), duplicated));
    ASSERT_TRUE(std::filesystem::remove(staging() / "data" / "signallab.sqlite3"));
    EXPECT_FALSE(source_.verify(staging(), snapshot.value()));
    EXPECT_FALSE(std::filesystem::exists(staging() / "data" / "signallab.sqlite3"));
}

TEST_F(ProjectBackupSourceTest, RejectsDatabaseWithForeignKeyViolations)
{
    QSqlQuery query(connection_.database());
    ASSERT_TRUE(query.exec("PRAGMA foreign_keys=OFF"));
    ASSERT_TRUE(query.exec(
        "INSERT INTO feature_result(task_id, feature_name, numeric_value, unit) "
        "VALUES (99999, 'broken', 1, 'V')"));
    query.finish();
    EXPECT_FALSE(source_.snapshot(staging()));
}

TEST_F(ProjectBackupSourceTest, RejectsIncompleteSchema)
{
    QSqlQuery query(connection_.database());
    ASSERT_TRUE(query.exec("DROP TABLE script_history"));
    query.finish();
    EXPECT_FALSE(source_.snapshot(staging()));
}

}
}
