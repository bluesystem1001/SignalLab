#include "database_backup_widget.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

#include "signallab/io/backup_settings.hpp"

namespace signallab::gui {

DatabaseBackupWidget::DatabaseBackupWidget(
    std::shared_ptr<app::BackupService> service,
    std::filesystem::path settingsPath,
    QWidget* parent)
    : QWidget(parent), service_(std::move(service)),
      settingsPath_(settingsPath.empty() ? io::defaultBackupSettingsPath() : std::move(settingsPath))
{
    setObjectName(QStringLiteral("databaseBackupWidget"));
    auto* layout = new QVBoxLayout(this);
    auto* introduction = new QLabel(QStringLiteral(
        "将项目数据库与导入的信号文件备份到 MariaDB。"
        "连接本机可使用 127.0.0.1；连接远端服务器时填写对应地址。"), this);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);

    auto* connectionGroup = new QGroupBox(QStringLiteral("MariaDB 连接配置"), this);
    connectionPanel_ = connectionGroup;
    auto* connectionLayout = new QGridLayout(connectionGroup);
    hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"), connectionGroup);
    hostEdit_->setObjectName(QStringLiteral("backupHostEdit"));
    hostEdit_->setMaxLength(255);
    portSpin_ = new QSpinBox(connectionGroup);
    portSpin_->setObjectName(QStringLiteral("backupPortSpin"));
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(3306);
    usernameEdit_ = new QLineEdit(QStringLiteral("root"), connectionGroup);
    usernameEdit_->setObjectName(QStringLiteral("backupUsernameEdit"));
    usernameEdit_->setMaxLength(128);
    passwordEdit_ = new QLineEdit(connectionGroup);
    passwordEdit_->setObjectName(QStringLiteral("backupPasswordEdit"));
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setMaxLength(4096);
    databaseEdit_ = new QLineEdit(QStringLiteral("signallab_backup"), connectionGroup);
    databaseEdit_->setObjectName(QStringLiteral("backupDatabaseEdit"));
    databaseEdit_->setMaxLength(64);
    rememberPasswordCheck_ = new QCheckBox(QStringLiteral("使用 Windows 用户加密记住密码"), connectionGroup);
    rememberPasswordCheck_->setObjectName(QStringLiteral("backupRememberPasswordCheck"));
#ifndef _WIN32
    rememberPasswordCheck_->setEnabled(false);
#endif
    connectionLayout->addWidget(new QLabel(QStringLiteral("服务器"), connectionGroup), 0, 0);
    connectionLayout->addWidget(hostEdit_, 0, 1);
    connectionLayout->addWidget(new QLabel(QStringLiteral("端口"), connectionGroup), 0, 2);
    connectionLayout->addWidget(portSpin_, 0, 3);
    connectionLayout->addWidget(new QLabel(QStringLiteral("用户名"), connectionGroup), 1, 0);
    connectionLayout->addWidget(usernameEdit_, 1, 1);
    connectionLayout->addWidget(new QLabel(QStringLiteral("密码"), connectionGroup), 1, 2);
    connectionLayout->addWidget(passwordEdit_, 1, 3);
    connectionLayout->addWidget(new QLabel(QStringLiteral("备份数据库"), connectionGroup), 2, 0);
    connectionLayout->addWidget(databaseEdit_, 2, 1);
    connectionLayout->addWidget(rememberPasswordCheck_, 2, 2, 1, 2);
    auto* passwordHint = new QLabel(QStringLiteral(
        "未勾选记住密码时，密码仅用于当前会话；配置保存在当前 Windows 用户目录。"), connectionGroup);
    passwordHint->setWordWrap(true);
    connectionLayout->addWidget(passwordHint, 3, 0, 1, 4);
    layout->addWidget(connectionGroup);

    auto* connectionButtons = new QHBoxLayout;
    testButton_ = new QPushButton(QStringLiteral("测试连接"), this);
    testButton_->setObjectName(QStringLiteral("backupTestConnectionButton"));
    saveSettingsButton_ = new QPushButton(QStringLiteral("保存配置"), this);
    saveSettingsButton_->setObjectName(QStringLiteral("backupSaveSettingsButton"));
    connectionButtons->addWidget(testButton_);
    connectionButtons->addWidget(saveSettingsButton_);
    connectionButtons->addStretch();
    layout->addLayout(connectionButtons);
    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("backupValidationLabel"));
    validationLabel_->setWordWrap(true);
    layout->addWidget(validationLabel_);

    auto* backupControls = new QHBoxLayout;
    backupLabelEdit_ = new QLineEdit(this);
    backupLabelEdit_->setObjectName(QStringLiteral("backupLabelEdit"));
    backupLabelEdit_->setPlaceholderText(QStringLiteral("备份名称（可选）"));
    backupLabelEdit_->setMaxLength(200);
    backupButton_ = new QPushButton(QStringLiteral("立即备份"), this);
    backupButton_->setObjectName(QStringLiteral("backupNowButton"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新备份列表"), this);
    refreshButton_->setObjectName(QStringLiteral("backupRefreshButton"));
    backupControls->addWidget(backupLabelEdit_, 1);
    backupControls->addWidget(backupButton_);
    backupControls->addWidget(refreshButton_);
    layout->addLayout(backupControls);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("backupRecordsTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("创建时间（UTC）"), QStringLiteral("备份名称"),
        QStringLiteral("文件数"), QStringLiteral("大小（MiB）")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->setMinimumHeight(150);
    layout->addWidget(table_, 1);

    auto* operationControls = new QHBoxLayout;
    restoreButton_ = new QPushButton(QStringLiteral("下载恢复副本…"), this);
    restoreButton_->setObjectName(QStringLiteral("backupRestoreButton"));
    cancelButton_ = new QPushButton(QStringLiteral("取消操作"), this);
    cancelButton_->setObjectName(QStringLiteral("backupCancelButton"));
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("backupProgressBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    operationControls->addWidget(restoreButton_);
    operationControls->addWidget(progressBar_, 1);
    operationControls->addWidget(cancelButton_);
    layout->addLayout(operationControls);

    statusLabel_ = new QLabel(QStringLiteral("配置连接后，可测试连接、创建备份或浏览已有备份。"), this);
    statusLabel_->setObjectName(QStringLiteral("backupStatusLabel"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statusLabel_);
    auto* restoreHint = new QLabel(QStringLiteral(
        "恢复副本会写入所选位置下的新目录，其中包含 data/signallab.sqlite3 与导入文件。"
        "如需恢复项目，请先关闭 SignalLab、另存原 data 目录，再用副本替换；当前项目不会被自动覆盖。"), this);
    restoreHint->setObjectName(QStringLiteral("backupRestoreHintLabel"));
    restoreHint->setWordWrap(true);
    layout->addWidget(restoreHint);

    connect(testButton_, &QPushButton::clicked, this, [this]() { testConnection(); });
    connect(saveSettingsButton_, &QPushButton::clicked, this, [this]() { saveSettings(); });
    connect(backupButton_, &QPushButton::clicked, this, [this]() { startBackup(); });
    connect(refreshButton_, &QPushButton::clicked, this, [this]() { refreshBackups(); });
    connect(restoreButton_, &QPushButton::clicked, this, [this]() { chooseRestoreDirectory(); });
    connect(cancelButton_, &QPushButton::clicked, this, [this]() { cancelOperation(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() { updateControls(); });
    for (auto* edit : {hostEdit_, usernameEdit_, passwordEdit_, databaseEdit_}) {
        connect(edit, &QLineEdit::textChanged, this, [this]() {
            table_->setRowCount(0);
            updateControls();
        });
    }
    connect(portSpin_, &QSpinBox::valueChanged, this, [this]() {
        table_->setRowCount(0);
        updateControls();
    });
    timer_ = new QTimer(this);
    timer_->setObjectName(QStringLiteral("backupPollTimer"));
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this]() { poll(); });
    timer_->start();
    loadSettings();
    updateControls();
}

DatabaseBackupWidget::~DatabaseBackupWidget()
{
    timer_->stop();
    stopSource_.request_stop();
    if (future_.valid()) {
        future_.wait();
    }
}

repository::BackupConnectionOptions DatabaseBackupWidget::connectionOptions() const
{
    repository::BackupConnectionOptions options;
    options.host = hostEdit_->text().trimmed().toStdString();
    options.port = static_cast<unsigned int>(portSpin_->value());
    options.username = usernameEdit_->text().trimmed().toStdString();
    options.password = passwordEdit_->text().toStdString();
    options.databaseName = databaseEdit_->text().trimmed().toStdString();
    return options;
}

QString DatabaseBackupWidget::validationMessage() const
{
    if (!service_) {
        return QStringLiteral("备份服务未配置。");
    }
    if (hostEdit_->text().trimmed().isEmpty() || usernameEdit_->text().trimmed().isEmpty()) {
        return QStringLiteral("请填写服务器地址与用户名。");
    }
    static const QRegularExpression databasePattern(QStringLiteral("^[A-Za-z0-9_]{1,64}$"));
    if (!databasePattern.match(databaseEdit_->text().trimmed()).hasMatch()) {
        return QStringLiteral("数据库名须为 1 至 64 个字母、数字或下划线。");
    }
    return {};
}

void DatabaseBackupWidget::loadSettings()
{
    std::error_code error;
    if (!std::filesystem::exists(settingsPath_, error) && !error) {
        return;
    }
    const auto loaded = io::loadBackupSettings(settingsPath_);
    if (!loaded) {
        statusLabel_->setText(QStringLiteral("加载连接配置失败：%1")
            .arg(QString::fromStdString(loaded.error().message)));
        return;
    }
    const auto& settings = loaded.value();
    hostEdit_->setText(QString::fromStdString(settings.connection.host));
    portSpin_->setValue(static_cast<int>(settings.connection.port));
    usernameEdit_->setText(QString::fromStdString(settings.connection.username));
    passwordEdit_->setText(QString::fromStdString(settings.connection.password));
    databaseEdit_->setText(QString::fromStdString(settings.connection.databaseName));
    rememberPasswordCheck_->setChecked(settings.rememberPassword);
    statusLabel_->setText(QStringLiteral("已加载连接配置；点击“测试连接”验证 MariaDB。"));
}

void DatabaseBackupWidget::saveSettings()
{
    if (future_.valid() || !validationMessage().isEmpty()) {
        return;
    }
    io::BackupSettings settings;
    settings.connection = connectionOptions();
    settings.rememberPassword = rememberPasswordCheck_->isChecked();
    const auto saved = io::saveBackupSettings(settingsPath_, settings);
    statusLabel_->setText(saved
        ? (settings.rememberPassword
            ? QStringLiteral("连接配置已保存，密码已使用当前 Windows 用户加密。")
            : QStringLiteral("连接配置已保存；密码未写入配置文件。"))
        : QStringLiteral("保存连接配置失败：%1").arg(QString::fromStdString(saved.error().message)));
}

bool DatabaseBackupWidget::launch(Operation operation, Worker worker)
{
    if (future_.valid() || !validationMessage().isEmpty()) {
        return false;
    }
    operation_ = operation;
    stopSource_ = std::stop_source();
    progress_ = std::make_shared<ProgressState>();
    try {
        future_ = std::async(std::launch::async,
            [worker = std::move(worker), token = stopSource_.get_token(), progress = progress_]() {
                return worker(token, progress);
            });
    } catch (const std::exception& error) {
        statusLabel_->setText(QStringLiteral("无法启动后台操作：%1").arg(QString::fromUtf8(error.what())));
        return false;
    }
    progressBar_->setRange(0, 0);
    updateControls();
    return true;
}

void DatabaseBackupWidget::testConnection()
{
    const auto service = service_;
    const auto connection = connectionOptions();
    if (launch(Operation::TestConnection, [service, connection](std::stop_token, std::shared_ptr<ProgressState>) {
            const auto tested = service->testConnection(connection);
            OperationResult result;
            result.success = static_cast<bool>(tested);
            result.message = tested ? tested.value() : tested.error().message;
            return result;
        })) {
        statusLabel_->setText(QStringLiteral("正在后台测试连接，请等待服务器响应…"));
    }
}

void DatabaseBackupWidget::refreshBackups()
{
    const auto service = service_;
    const auto connection = connectionOptions();
    if (launch(Operation::List, [service, connection](std::stop_token, std::shared_ptr<ProgressState>) {
            auto listed = service->list(connection);
            OperationResult result;
            result.success = static_cast<bool>(listed);
            if (listed) {
                result.records = std::move(listed).value();
            } else {
                result.message = listed.error().message;
            }
            return result;
        })) {
        statusLabel_->setText(QStringLiteral("正在后台读取备份列表…"));
    }
}

void DatabaseBackupWidget::startBackup()
{
    const auto service = service_;
    const auto connection = connectionOptions();
    const std::string label = backupLabelEdit_->text().trimmed().toStdString();
    if (launch(Operation::Backup, [service, connection, label](std::stop_token token, std::shared_ptr<ProgressState> progress) {
            const auto backedUp = service->backup(connection, label, token,
                [progress](std::uint64_t completed, std::uint64_t total) {
                    progress->completed.store(completed);
                    progress->total.store(total);
                });
            OperationResult result;
            result.success = static_cast<bool>(backedUp);
            if (!backedUp) {
                result.message = backedUp.error().message;
                return result;
            }
            result.selectedId = backedUp.value().id;
            auto listed = service->list(connection);
            if (listed) {
                result.records = std::move(listed).value();
            } else {
                result.message = listed.error().message;
            }
            return result;
        })) {
        statusLabel_->setText(QStringLiteral("正在创建一致性快照并上传备份…"));
    }
}

std::string DatabaseBackupWidget::selectedBackupId() const
{
    const auto* item = table_->item(table_->currentRow(), 0);
    return item == nullptr ? std::string() : item->data(Qt::UserRole).toString().toStdString();
}

void DatabaseBackupWidget::chooseRestoreDirectory()
{
    if (future_.valid() || selectedBackupId().empty()) {
        return;
    }
    const QString parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择恢复副本的保存位置"));
    if (parent.isEmpty()) {
        return;
    }
    const QString name = QStringLiteral("SignalLab-restore-%1-%2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
        .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    const auto destination = std::filesystem::path(parent.toStdWString()) / name.toStdWString();
    static_cast<void>(restoreToDirectory(destination));
}

bool DatabaseBackupWidget::restoreToDirectory(const std::filesystem::path& destination)
{
    if (destination.empty() || selectedBackupId().empty()) {
        return false;
    }
    const auto service = service_;
    const auto connection = connectionOptions();
    const auto backupId = selectedBackupId();
    if (!launch(Operation::Restore, [service, connection, backupId, destination](
                std::stop_token token, std::shared_ptr<ProgressState> progress) {
            const auto restored = service->restoreCopy(connection, backupId, destination, token,
                [progress](std::uint64_t completed, std::uint64_t total) {
                    progress->completed.store(completed);
                    progress->total.store(total);
                });
            OperationResult result;
            result.success = static_cast<bool>(restored);
            if (restored) {
                result.restoredPath = restored.value();
            } else {
                result.message = restored.error().message;
            }
            return result;
        })) {
        return false;
    }
    statusLabel_->setText(QStringLiteral("正在下载并校验恢复副本；当前项目保持不变…"));
    return true;
}

void DatabaseBackupWidget::cancelOperation()
{
    if (!future_.valid()) {
        return;
    }
    stopSource_.request_stop();
    statusLabel_->setText(QStringLiteral("已请求取消，正在等待当前数据库操作安全结束…"));
    updateControls();
}

void DatabaseBackupWidget::showRecords(
    const std::vector<repository::BackupRecord>& records,
    const std::string& selectedId)
{
    table_->setRowCount(0);
    for (const auto& record : records) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* created = new QTableWidgetItem(QString::fromStdString(record.createdUtc));
        created->setData(Qt::UserRole, QString::fromStdString(record.id));
        created->setToolTip(QString::fromStdString(record.id));
        table_->setItem(row, 0, created);
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(record.label)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::number(record.fileCount)));
        table_->setItem(row, 3, new QTableWidgetItem(QString::number(
            static_cast<double>(record.totalBytes) / (1024.0 * 1024.0), 'f', 2)));
        if (record.id == selectedId) {
            table_->setCurrentCell(row, 0);
        }
    }
    if (table_->currentRow() < 0 && table_->rowCount() > 0) {
        table_->setCurrentCell(0, 0);
    }
}

void DatabaseBackupWidget::poll()
{
    if (!future_.valid()) {
        return;
    }
    const auto total = progress_->total.load();
    if (total > 0) {
        const auto completed = std::min(progress_->completed.load(), total);
        progressBar_->setRange(0, 100);
        progressBar_->setValue(static_cast<int>(
            100.0L * static_cast<long double>(completed) / static_cast<long double>(total)));
    }
    if (future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    try {
        const auto result = future_.get();
        if (result.records) {
            showRecords(*result.records, result.selectedId);
        }
        if (!result.success) {
            statusLabel_->setText(QStringLiteral("操作未完成：%1").arg(QString::fromStdString(result.message)));
        } else if (operation_ == Operation::TestConnection) {
            statusLabel_->setText(QStringLiteral("连接成功：%1").arg(QString::fromStdString(result.message)));
        } else if (operation_ == Operation::List) {
            statusLabel_->setText(QStringLiteral("已加载 %1 份备份。").arg(table_->rowCount()));
        } else if (operation_ == Operation::Backup) {
            statusLabel_->setText(result.message.empty()
                ? QStringLiteral("备份成功，备份列表已刷新。")
                : QStringLiteral("备份成功，但刷新列表失败：%1").arg(QString::fromStdString(result.message)));
        } else if (result.restoredPath) {
            statusLabel_->setText(QStringLiteral("恢复副本已下载并校验：%1\n请先关闭 SignalLab、另存原 data 目录，再手动替换。")
                .arg(QString::fromStdWString(result.restoredPath->wstring())));
        }
        progressBar_->setRange(0, 100);
        progressBar_->setValue(result.success ? 100 : 0);
    } catch (const std::exception& error) {
        statusLabel_->setText(QStringLiteral("后台操作失败：%1").arg(QString::fromUtf8(error.what())));
        progressBar_->setRange(0, 100);
        progressBar_->setValue(0);
    }
    updateControls();
}

void DatabaseBackupWidget::updateControls()
{
    const bool busy = future_.valid();
    const QString validation = validationMessage();
    connectionPanel_->setEnabled(!busy);
    backupLabelEdit_->setEnabled(!busy);
    table_->setEnabled(!busy);
    testButton_->setEnabled(!busy && validation.isEmpty());
    saveSettingsButton_->setEnabled(!busy && validation.isEmpty());
    backupButton_->setEnabled(!busy && validation.isEmpty());
    refreshButton_->setEnabled(!busy && validation.isEmpty());
    restoreButton_->setEnabled(!busy && validation.isEmpty() && !selectedBackupId().empty());
    cancelButton_->setEnabled(busy && !stopSource_.stop_requested()
        && (operation_ == Operation::Backup || operation_ == Operation::Restore));
    validationLabel_->setText(validation);
    validationLabel_->setVisible(!validation.isEmpty());
}

bool DatabaseBackupWidget::requestClose()
{
    if (!future_.valid()) {
        return true;
    }
    if (operation_ == Operation::TestConnection || operation_ == Operation::List) {
        QMessageBox::information(this, QStringLiteral("数据库请求尚未结束"),
            QStringLiteral("当前连接请求需要等待服务器响应；请求结束后即可关闭窗口。"));
        return false;
    }
    if (QMessageBox::question(this, QStringLiteral("后台备份操作尚未结束"),
            QStringLiteral("是否请求取消？请等待当前请求安全结束后再关闭窗口。"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) {
        cancelOperation();
    }
    return false;
}

void DatabaseBackupWidget::closeEvent(QCloseEvent* event)
{
    if (requestClose()) {
        event->accept();
    } else {
        event->ignore();
    }
}

}
