#include <QDateTime>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/app/backup_service.hpp"

namespace signallab::app {
namespace {

using Files = std::vector<repository::BackupFile>;

Files stageFakeProject(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory / "data" / "imports");
    const std::string importedPath = "data/imports/" + std::string(64, 'a') + ".source";
    const std::vector<std::pair<std::string, std::string>> contents{
        {"data/signallab.sqlite3", "database"},
        {importedPath, "source"}
    };
    Files files;
    for (const auto& [relativePath, bytes] : contents) {
        const auto path = directory / relativePath;
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        EXPECT_TRUE(output.good());
        files.push_back({relativePath, path, bytes.size(), std::string(64, 'a')});
    }
    return files;
}

class FakeBackupSource final : public repository::BackupSource {
public:
    core::Result<Files> snapshot(
        const std::filesystem::path& stagingDirectory,
        std::stop_token) override
    {
        ++snapshotCalls;
        snapshotDirectory = stagingDirectory;
        auto files = stageFakeProject(stagingDirectory);
        if (onSnapshot) {
            onSnapshot();
        }
        if (failSnapshot) {
            return core::Result<Files>::failure(
                core::ErrorCode::FileNotFound, "Missing imported source.");
        }
        return core::Result<Files>::success(std::move(files));
    }

    core::Result<bool> verify(
        const std::filesystem::path& directory,
        const Files& files,
        std::stop_token) override
    {
        ++verifyCalls;
        verifyDirectory = directory;
        EXPECT_EQ(files.size(), 2U);
        for (const auto& file : files) {
            EXPECT_TRUE(std::filesystem::exists(file.localPath));
            EXPECT_EQ(file.localPath, directory / file.relativePath);
        }
        if (onVerify) {
            onVerify();
        }
        if (failVerify) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter, "Restored database is corrupt.");
        }
        return core::Result<bool>::success(true);
    }

    bool failSnapshot{false};
    bool failVerify{false};
    int snapshotCalls{0};
    int verifyCalls{0};
    std::filesystem::path snapshotDirectory;
    std::filesystem::path verifyDirectory;
    std::function<void()> onSnapshot;
    std::function<void()> onVerify;
};

class FakeBackupRepository final : public repository::BackupRepository {
public:
    core::Result<std::string> testConnection(
        const repository::BackupConnectionOptions&) override
    {
        return core::Result<std::string>::success("MariaDB test server");
    }

    core::Result<repository::BackupRecord> upload(
        const repository::BackupConnectionOptions& options,
        repository::BackupRecord record,
        const Files& files,
        std::stop_token stopToken,
        repository::BackupProgress progress) override
    {
        ++uploadCalls;
        uploadedOptions = options;
        uploadedRecord = record;
        uploadedFiles = files;
        EXPECT_FALSE(stopToken.stop_requested());
        for (const auto& file : files) {
            EXPECT_TRUE(std::filesystem::exists(file.localPath));
        }
        if (progress) {
            progress(record.totalBytes, record.totalBytes);
        }
        if (failUpload) {
            return core::Result<repository::BackupRecord>::failure(
                core::ErrorCode::DatabaseQueryFailed, "Upload transaction failed.");
        }
        return core::Result<repository::BackupRecord>::success(std::move(record));
    }

    core::Result<std::vector<repository::BackupRecord>> list(
        const repository::BackupConnectionOptions&) override
    {
        return core::Result<std::vector<repository::BackupRecord>>::success({});
    }

    core::Result<Files> download(
        const repository::BackupConnectionOptions&,
        const std::string& backupId,
        const std::filesystem::path& stagingDirectory,
        std::stop_token,
        repository::BackupProgress progress) override
    {
        ++downloadCalls;
        downloadedId = backupId;
        downloadDirectory = stagingDirectory;
        auto files = stageFakeProject(stagingDirectory);
        if (progress) {
            progress(14, 14);
        }
        if (failDownload) {
            return core::Result<Files>::failure(
                core::ErrorCode::DatabaseQueryFailed, "Download connection failed.");
        }
        return core::Result<Files>::success(std::move(files));
    }

    bool failUpload{false};
    bool failDownload{false};
    int uploadCalls{0};
    int downloadCalls{0};
    repository::BackupConnectionOptions uploadedOptions;
    repository::BackupRecord uploadedRecord;
    Files uploadedFiles;
    std::string downloadedId;
    std::filesystem::path downloadDirectory;
};

class BackupServiceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(temporary_.isValid());
    }

    std::filesystem::path destination() const
    {
        return std::filesystem::path(temporary_.path().toStdWString()) / "restored-project";
    }

    QTemporaryDir temporary_;
    std::shared_ptr<FakeBackupSource> source_ = std::make_shared<FakeBackupSource>();
    std::shared_ptr<FakeBackupRepository> repository_ = std::make_shared<FakeBackupRepository>();
    BackupService service_{source_, repository_};
    repository::BackupConnectionOptions options_;
};

TEST_F(BackupServiceTest, CleansPartialSnapshotAndUploadFailureStaging)
{
    source_->failSnapshot = true;
    const auto snapshotFailure = service_.backup(options_, "snapshot failure");
    ASSERT_FALSE(snapshotFailure);
    EXPECT_EQ(snapshotFailure.error().code, core::ErrorCode::FileNotFound);
    EXPECT_EQ(repository_->uploadCalls, 0);
    ASSERT_FALSE(source_->snapshotDirectory.empty());
    EXPECT_FALSE(std::filesystem::exists(source_->snapshotDirectory));

    source_->failSnapshot = false;
    repository_->failUpload = true;
    const auto uploadFailure = service_.backup(options_, "upload failure");
    ASSERT_FALSE(uploadFailure);
    EXPECT_EQ(uploadFailure.error().code, core::ErrorCode::DatabaseQueryFailed);
    EXPECT_EQ(repository_->uploadCalls, 1);
    EXPECT_FALSE(std::filesystem::exists(source_->snapshotDirectory));
}

TEST_F(BackupServiceTest, CancellationBeforeAndDuringSnapshotNeverUploads)
{
    std::stop_source earlyStop;
    earlyStop.request_stop();
    const auto early = service_.backup(options_, "cancelled", earlyStop.get_token());
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().code, core::ErrorCode::Cancelled);
    EXPECT_EQ(source_->snapshotCalls, 0);

    std::stop_source duringSnapshot;
    source_->onSnapshot = [&duringSnapshot]() { duringSnapshot.request_stop(); };
    const auto cancelled = service_.backup(options_, "cancelled", duringSnapshot.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, core::ErrorCode::Cancelled);
    EXPECT_EQ(repository_->uploadCalls, 0);
    EXPECT_FALSE(std::filesystem::exists(source_->snapshotDirectory));
}

TEST_F(BackupServiceTest, UploadsSnapshotWithRecordMetadataOptionsAndProgress)
{
    options_.host = "localhost";
    options_.port = 3307;
    options_.username = "backup_user";
    options_.password = "test-only-password";
    options_.databaseName = "backup_test";
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    const auto result = service_.backup(options_, "before analysis", {},
        [&](std::uint64_t completedBytes, std::uint64_t totalBytes) {
            completed = completedBytes;
            total = totalBytes;
        });
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(repository_->uploadCalls, 1);
    EXPECT_EQ(result.value().id.size(), 36U);
    EXPECT_EQ(result.value().id, repository_->uploadedRecord.id);
    EXPECT_TRUE(QDateTime::fromString(
        QString::fromStdString(result.value().createdUtc), Qt::ISODate).isValid());
    EXPECT_TRUE(result.value().createdUtc.ends_with('Z'));
    EXPECT_EQ(result.value().label, "before analysis");
    EXPECT_EQ(result.value().sourceName, "SignalLab");
    EXPECT_EQ(result.value().fileCount, 2U);
    EXPECT_EQ(result.value().totalBytes, 14U);
    EXPECT_EQ(completed, 14U);
    EXPECT_EQ(total, 14U);
    EXPECT_EQ(repository_->uploadedOptions.host, options_.host);
    EXPECT_EQ(repository_->uploadedOptions.port, options_.port);
    EXPECT_EQ(repository_->uploadedOptions.username, options_.username);
    EXPECT_EQ(repository_->uploadedOptions.password, options_.password);
    EXPECT_EQ(repository_->uploadedOptions.databaseName, options_.databaseName);
    EXPECT_FALSE(std::filesystem::exists(source_->snapshotDirectory));
}

TEST_F(BackupServiceTest, RestoresOnlyAfterVerificationAndPromotesEntireDirectory)
{
    source_->onVerify = [this]() {
        EXPECT_FALSE(std::filesystem::exists(destination()));
        EXPECT_TRUE(std::filesystem::exists(repository_->downloadDirectory));
    };
    std::uint64_t completed = 0;
    const auto result = service_.restoreCopy(options_, "backup-to-restore", destination(), {},
        [&](std::uint64_t completedBytes, std::uint64_t) { completed = completedBytes; });
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value(), std::filesystem::canonical(destination()));
    EXPECT_EQ(repository_->downloadedId, "backup-to-restore");
    EXPECT_EQ(source_->verifyCalls, 1);
    EXPECT_EQ(source_->verifyDirectory, repository_->downloadDirectory);
    EXPECT_NE(repository_->downloadDirectory, destination());
    EXPECT_FALSE(std::filesystem::exists(repository_->downloadDirectory));
    EXPECT_TRUE(std::filesystem::exists(destination() / "data" / "signallab.sqlite3"));
    EXPECT_TRUE(std::filesystem::exists(destination() / "data" / "imports"
        / (std::string(64, 'a') + ".source")));
    EXPECT_EQ(completed, 14U);
}

TEST_F(BackupServiceTest, NeverReplacesExistingOrConcurrentlyCreatedDestination)
{
    std::filesystem::create_directory(destination());
    const auto existing = service_.restoreCopy(options_, "backup", destination());
    ASSERT_FALSE(existing);
    EXPECT_EQ(repository_->downloadCalls, 0);
    EXPECT_TRUE(std::filesystem::is_empty(destination()));
    std::filesystem::remove(destination());

    source_->onVerify = [this]() { std::filesystem::create_directory(destination()); };
    const auto concurrent = service_.restoreCopy(options_, "backup", destination());
    ASSERT_FALSE(concurrent);
    EXPECT_TRUE(std::filesystem::is_empty(destination()));
    EXPECT_FALSE(std::filesystem::exists(repository_->downloadDirectory));
}

TEST_F(BackupServiceTest, CleansFailedDownloadAndCorruptOrCancelledRestore)
{
    repository_->failDownload = true;
    const auto downloadFailure = service_.restoreCopy(options_, "backup", destination());
    ASSERT_FALSE(downloadFailure);
    EXPECT_EQ(source_->verifyCalls, 0);
    EXPECT_FALSE(std::filesystem::exists(destination()));
    EXPECT_FALSE(std::filesystem::exists(repository_->downloadDirectory));

    repository_->failDownload = false;
    source_->failVerify = true;
    const auto corrupt = service_.restoreCopy(options_, "backup", destination());
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_FALSE(std::filesystem::exists(destination()));
    EXPECT_FALSE(std::filesystem::exists(repository_->downloadDirectory));

    source_->failVerify = false;
    std::stop_source stop;
    source_->onVerify = [&stop]() { stop.request_stop(); };
    const auto cancelled = service_.restoreCopy(options_, "backup", destination(), stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, core::ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(destination()));
    EXPECT_FALSE(std::filesystem::exists(repository_->downloadDirectory));
}

}
}
