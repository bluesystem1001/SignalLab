#pragma once

#include <QWidget>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "signallab/app/backup_service.hpp"

class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace signallab::gui {

class DatabaseBackupWidget final : public QWidget
{
public:
    explicit DatabaseBackupWidget(
        std::shared_ptr<app::BackupService> service,
        std::filesystem::path settingsPath = {},
        QWidget* parent = nullptr);
    ~DatabaseBackupWidget() override;

    [[nodiscard]] bool requestClose();
    [[nodiscard]] bool restoreToDirectory(const std::filesystem::path& destination);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Operation { TestConnection, List, Backup, Restore };

    struct ProgressState {
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> total{0};
    };

    struct OperationResult {
        bool success{false};
        std::string message;
        std::optional<std::vector<repository::BackupRecord>> records;
        std::string selectedId;
        std::optional<std::filesystem::path> restoredPath;
    };

    using Worker = std::function<OperationResult(
        std::stop_token, std::shared_ptr<ProgressState>)>;

    [[nodiscard]] repository::BackupConnectionOptions connectionOptions() const;
    [[nodiscard]] QString validationMessage() const;
    [[nodiscard]] std::string selectedBackupId() const;
    [[nodiscard]] bool launch(Operation operation, Worker worker);
    void loadSettings();
    void saveSettings();
    void testConnection();
    void refreshBackups();
    void startBackup();
    void chooseRestoreDirectory();
    void cancelOperation();
    void poll();
    void updateControls();
    void showRecords(const std::vector<repository::BackupRecord>& records,
                     const std::string& selectedId);

    std::shared_ptr<app::BackupService> service_;
    std::filesystem::path settingsPath_;
    std::future<OperationResult> future_;
    std::stop_source stopSource_;
    std::shared_ptr<ProgressState> progress_;
    Operation operation_{Operation::List};

    QWidget* connectionPanel_{nullptr};
    QLineEdit* hostEdit_{nullptr};
    QSpinBox* portSpin_{nullptr};
    QLineEdit* usernameEdit_{nullptr};
    QLineEdit* passwordEdit_{nullptr};
    QLineEdit* databaseEdit_{nullptr};
    QCheckBox* rememberPasswordCheck_{nullptr};
    QPushButton* testButton_{nullptr};
    QPushButton* saveSettingsButton_{nullptr};
    QLineEdit* backupLabelEdit_{nullptr};
    QPushButton* backupButton_{nullptr};
    QPushButton* refreshButton_{nullptr};
    QPushButton* restoreButton_{nullptr};
    QPushButton* cancelButton_{nullptr};
    QTableWidget* table_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* validationLabel_{nullptr};
    QLabel* statusLabel_{nullptr};
    QTimer* timer_{nullptr};
};

}
