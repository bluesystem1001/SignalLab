#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gui/database_backup_widget.hpp"
#include "signallab/io/backup_settings.hpp"

namespace {

std::vector<signallab::repository::BackupFile> writeTestSnapshot(
    const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory / "data" / "imports");
    const auto database = directory / "data" / "signallab.sqlite3";
    const auto signal = directory / "data" / "imports" / "input.wav";
    std::ofstream(database, std::ios::binary) << "fake database";
    std::ofstream(signal, std::ios::binary) << "fake signal";
    return {{"data/signallab.sqlite3", database, 13U, "database-hash"},
            {"data/imports/input.wav", signal, 11U, "signal-hash"}};
}

class FakeBackupSource final : public signallab::repository::BackupSource {
public:
    signallab::core::Result<std::vector<signallab::repository::BackupFile>> snapshot(
        const std::filesystem::path& directory, std::stop_token) override
    {
        return signallab::core::Result<std::vector<signallab::repository::BackupFile>>::success(
            writeTestSnapshot(directory));
    }

    signallab::core::Result<bool> verify(
        const std::filesystem::path& directory,
        const std::vector<signallab::repository::BackupFile>& files,
        std::stop_token) override
    {
        ++verifyCount;
        return signallab::core::Result<bool>::success(
            files.size() == 2U && std::filesystem::exists(directory / "data" / "signallab.sqlite3"));
    }

    std::atomic<int> verifyCount{0};
};

class FakeBackupRepository final : public signallab::repository::BackupRepository {
public:
    signallab::core::Result<std::string> testConnection(
        const signallab::repository::BackupConnectionOptions& options) override
    {
        lastConnection = options;
        if (failConnection) {
            return signallab::core::Result<std::string>::failure(
                signallab::core::ErrorCode::DatabaseOpenFailed, "Server unavailable");
        }
        return signallab::core::Result<std::string>::success("MariaDB fake server");
    }

    signallab::core::Result<signallab::repository::BackupRecord> upload(
        const signallab::repository::BackupConnectionOptions&,
        signallab::repository::BackupRecord record,
        const std::vector<signallab::repository::BackupFile>&,
        std::stop_token token,
        signallab::repository::BackupProgress progress) override
    {
        if (progress) {
            progress(32U, 64U);
        }
        uploadStarted.store(true);
        while (pauseUpload.load() && !token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (token.stop_requested()) {
            sawCancellation.store(true);
            return signallab::core::Result<signallab::repository::BackupRecord>::failure(
                signallab::core::ErrorCode::Cancelled, "Backup cancelled");
        }
        records.push_back(record);
        return signallab::core::Result<signallab::repository::BackupRecord>::success(std::move(record));
    }

    signallab::core::Result<std::vector<signallab::repository::BackupRecord>> list(
        const signallab::repository::BackupConnectionOptions&) override
    {
        return signallab::core::Result<std::vector<signallab::repository::BackupRecord>>::success(records);
    }

    signallab::core::Result<std::vector<signallab::repository::BackupFile>> download(
        const signallab::repository::BackupConnectionOptions&,
        const std::string&,
        const std::filesystem::path& stagingDirectory,
        std::stop_token,
        signallab::repository::BackupProgress progress) override
    {
        if (progress) {
            progress(24U, 24U);
        }
        return signallab::core::Result<std::vector<signallab::repository::BackupFile>>::success(
            writeTestSnapshot(stagingDirectory));
    }

    bool failConnection{false};
    std::atomic<bool> pauseUpload{false};
    std::atomic<bool> uploadStarted{false};
    std::atomic<bool> sawCancellation{false};
    signallab::repository::BackupConnectionOptions lastConnection;
    std::vector<signallab::repository::BackupRecord> records;
};

class DatabaseBackupWidgetTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        settingsPath = std::filesystem::path(directory.path().toStdWString()) / "connection.json";
        source = std::make_shared<FakeBackupSource>();
        repository = std::make_shared<FakeBackupRepository>();
        service = std::make_shared<signallab::app::BackupService>(source, repository);
        widget = std::make_unique<signallab::gui::DatabaseBackupWidget>(service, settingsPath);
    }

    template<typename Widget>
    Widget* control(const char* name)
    {
        return widget->findChild<Widget*>(QString::fromLatin1(name));
    }

    void poll()
    {
        QMetaObject::invokeMethod(control<QTimer>("backupPollTimer"), "timeout", Qt::DirectConnection);
    }

    template<typename Predicate>
    bool waitUntil(Predicate predicate)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!predicate() && elapsed.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            poll();
            QThread::msleep(1);
        }
        return predicate();
    }

    bool waitForCompletion()
    {
        return waitUntil([this]() { return control<QPushButton>("backupNowButton")->isEnabled(); });
    }

    QTemporaryDir directory;
    std::filesystem::path settingsPath;
    std::shared_ptr<FakeBackupSource> source;
    std::shared_ptr<FakeBackupRepository> repository;
    std::shared_ptr<signallab::app::BackupService> service;
    std::unique_ptr<signallab::gui::DatabaseBackupWidget> widget;
};

TEST_F(DatabaseBackupWidgetTest, DefaultsToLocalMariaDbAndMasksThePassword)
{
    EXPECT_EQ(control<QLineEdit>("backupHostEdit")->text(), QStringLiteral("127.0.0.1"));
    EXPECT_EQ(control<QSpinBox>("backupPortSpin")->value(), 3306);
    EXPECT_EQ(control<QLineEdit>("backupUsernameEdit")->text(), QStringLiteral("root"));
    EXPECT_EQ(control<QLineEdit>("backupDatabaseEdit")->text(), QStringLiteral("signallab_backup"));
    EXPECT_EQ(control<QLineEdit>("backupPasswordEdit")->echoMode(), QLineEdit::Password);
    EXPECT_FALSE(control<QCheckBox>("backupRememberPasswordCheck")->isChecked());
    EXPECT_FALSE(control<QPushButton>("backupRestoreButton")->isEnabled());
    EXPECT_TRUE(widget->requestClose());
}

TEST_F(DatabaseBackupWidgetTest, InvalidConnectionFieldsDisableRequests)
{
    control<QLineEdit>("backupHostEdit")->clear();
    EXPECT_FALSE(control<QPushButton>("backupTestConnectionButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("backupNowButton")->isEnabled());
    control<QLineEdit>("backupHostEdit")->setText(QStringLiteral("localhost"));
    control<QLineEdit>("backupDatabaseEdit")->setText(QStringLiteral("backup; DROP TABLE backup"));
    EXPECT_FALSE(control<QPushButton>("backupRefreshButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("backupValidationLabel")->text().contains(QStringLiteral("数据库名")));
    control<QLineEdit>("backupDatabaseEdit")->setText(QStringLiteral("project_backup"));
    EXPECT_TRUE(control<QPushButton>("backupRefreshButton")->isEnabled());
}

TEST_F(DatabaseBackupWidgetTest, SavesAndLoadsConfigurationWithoutRememberingThePassword)
{
    control<QLineEdit>("backupHostEdit")->setText(QStringLiteral("localhost"));
    control<QLineEdit>("backupUsernameEdit")->setText(QStringLiteral("backup_user"));
    control<QLineEdit>("backupPasswordEdit")->setText(QStringLiteral("widget-test-password"));
    control<QPushButton>("backupSaveSettingsButton")->click();
    const auto settings = signallab::io::loadBackupSettings(settingsPath);
    ASSERT_TRUE(settings);
    EXPECT_EQ(settings.value().connection.host, "localhost");
    EXPECT_TRUE(settings.value().connection.password.empty());
    QFile saved(QString::fromStdWString(settingsPath.wstring()));
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_FALSE(saved.readAll().contains("widget-test-password"));
    widget = std::make_unique<signallab::gui::DatabaseBackupWidget>(service, settingsPath);
    EXPECT_EQ(control<QLineEdit>("backupUsernameEdit")->text(), QStringLiteral("backup_user"));
    EXPECT_TRUE(control<QLineEdit>("backupPasswordEdit")->text().isEmpty());
}

TEST_F(DatabaseBackupWidgetTest, ConnectionTestRunsAsynchronouslyAndReportsFailures)
{
    repository->failConnection = true;
    control<QPushButton>("backupTestConnectionButton")->click();
    EXPECT_FALSE(control<QLineEdit>("backupHostEdit")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("backupRefreshButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("backupCancelButton")->isEnabled());
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("Server unavailable")));
    repository->failConnection = false;
    control<QPushButton>("backupTestConnectionButton")->click();
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("连接成功")));
}

TEST_F(DatabaseBackupWidgetTest, BackupRefreshesTheListAndEnablesRestore)
{
    control<QLineEdit>("backupLabelEdit")->setText(QStringLiteral("GUI 测试备份"));
    control<QPushButton>("backupNowButton")->click();
    EXPECT_FALSE(control<QPushButton>("backupSaveSettingsButton")->isEnabled());
    ASSERT_TRUE(waitForCompletion());
    ASSERT_EQ(control<QTableWidget>("backupRecordsTable")->rowCount(), 1);
    EXPECT_EQ(control<QTableWidget>("backupRecordsTable")->item(0, 1)->text(), QStringLiteral("GUI 测试备份"));
    EXPECT_EQ(control<QTableWidget>("backupRecordsTable")->item(0, 2)->text(), QStringLiteral("2"));
    EXPECT_TRUE(control<QPushButton>("backupRestoreButton")->isEnabled());
    EXPECT_EQ(control<QProgressBar>("backupProgressBar")->value(), 100);
}

TEST_F(DatabaseBackupWidgetTest, CancelsUploadWithoutLeavingTheControlsBusy)
{
    repository->pauseUpload.store(true);
    control<QPushButton>("backupNowButton")->click();
    ASSERT_TRUE(waitUntil([this]() { return repository->uploadStarted.load(); }));
    poll();
    EXPECT_EQ(control<QProgressBar>("backupProgressBar")->value(), 50);
    EXPECT_TRUE(control<QPushButton>("backupCancelButton")->isEnabled());
    control<QPushButton>("backupCancelButton")->click();
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(repository->sawCancellation.load());
    EXPECT_EQ(control<QTableWidget>("backupRecordsTable")->rowCount(), 0);
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("cancelled")));
}

TEST_F(DatabaseBackupWidgetTest, DefersClosingUntilAnUploadHasCancelled)
{
    repository->pauseUpload.store(true);
    control<QPushButton>("backupNowButton")->click();
    ASSERT_TRUE(waitUntil([this]() { return repository->uploadStarted.load(); }));
    QTimer::singleShot(0, widget.get(), []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto* confirmButton = dialog->button(QMessageBox::Yes);
        ASSERT_NE(confirmButton, nullptr);
        confirmButton->click();
    });
    EXPECT_FALSE(widget->requestClose());
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(widget->requestClose());
    EXPECT_TRUE(repository->sawCancellation.load());
}

TEST_F(DatabaseBackupWidgetTest, RestoresIntoANewUnicodeDirectoryAndVerifiesTheCopy)
{
    control<QPushButton>("backupNowButton")->click();
    ASSERT_TRUE(waitForCompletion());
    const auto destination = std::filesystem::path(directory.path().toStdWString())
        / std::filesystem::path(QStringLiteral("恢复副本").toStdWString());
    ASSERT_TRUE(widget->restoreToDirectory(destination));
    EXPECT_FALSE(control<QPushButton>("backupNowButton")->isEnabled());
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(std::filesystem::exists(destination / "data" / "signallab.sqlite3"));
    EXPECT_TRUE(std::filesystem::exists(destination / "data" / "imports" / "input.wav"));
    EXPECT_EQ(source->verifyCount.load(), 1);
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("手动替换")));
    ASSERT_TRUE(widget->restoreToDirectory(destination));
    ASSERT_TRUE(waitForCompletion());
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("already exists")));
    EXPECT_EQ(source->verifyCount.load(), 1);
}

TEST_F(DatabaseBackupWidgetTest, ShowsConfigurationLoadErrorsWithoutOpeningTheServer)
{
    std::ofstream(settingsPath) << "not valid JSON";
    widget = std::make_unique<signallab::gui::DatabaseBackupWidget>(service, settingsPath);
    EXPECT_TRUE(control<QLabel>("backupStatusLabel")->text().contains(QStringLiteral("加载连接配置失败")));
    EXPECT_TRUE(control<QPushButton>("backupTestConnectionButton")->isEnabled());
}

}
