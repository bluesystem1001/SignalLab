#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QString>

#include <filesystem>
#include <system_error>
#include <utility>

#include "main_window.hpp"
#include "signallab/app/analysis_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/core/signal_generator.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"
#include "signallab/io/filesystem_file_repository.hpp"

namespace {

std::filesystem::path findProjectRoot()
{
    const auto applicationDirectory = std::filesystem::path(
        QCoreApplication::applicationDirPath().toStdWString());
    auto candidate = applicationDirectory;

    while (!candidate.empty()) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "CMakeLists.txt", error)
            && !error) {
            return candidate;
        }

        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }

    // A packaged program stores its data beside the executable.
    return applicationDirectory;
}

void showStartupError(const std::string& message)
{
    QMessageBox::critical(
        nullptr,
        "SignalLab 启动失败",
        QString::fromStdString(message));
}

}  // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    const std::filesystem::path projectRoot = findProjectRoot();

    signallab::database::DatabaseConnection database(
        projectRoot / "data" / "signallab.sqlite3");
    auto openResult = database.open();
    if (!openResult) {
        showStartupError(openResult.error().message);
        return 1;
    }

    signallab::database::MigrationRunner migrationRunner(database);
    auto migrationResult = migrationRunner.run(projectRoot / "migrations");
    if (!migrationResult) {
        showStartupError(migrationResult.error().message);
        return 1;
    }

    signallab::database::SqliteSignalRepository signalRepository(database);
    signallab::database::SqliteTaskRepository taskRepository(database);
    signallab::io::FilesystemFileRepository fileRepository(projectRoot);
    signallab::app::ImportService importService(
        fileRepository,
        signalRepository);
    signallab::app::AnalysisService analysisService(
        taskRepository,
        signalRepository);

    auto interruptionResult = analysisService.interruptRunningTasks();
    if (!interruptionResult) {
        showStartupError(interruptionResult.error().message);
        return 1;
    }

    auto restoreResult = importService.restoreSignals();
    if (!restoreResult) {
        showStartupError(restoreResult.error().message);
        return 1;
    }

    signallab::core::SignalMetadata metadata;
    metadata.name = "1 kHz 正弦信号";
    metadata.unit = "幅值";
    metadata.source = "内置模拟器";

    auto signalResult = signallab::core::generateSineWave(
        1000.0,
        1.0,
        48000.0,
        0.02,
        metadata);
    if (!signalResult) {
        showStartupError(signalResult.error().message);
        return 1;
    }

    auto generatedResult = importService.addGeneratedSignal(
        std::move(signalResult).value(),
        "builtin:sine:frequency=1000;amplitude=1;sample_rate=48000;duration=0.02");
    if (!generatedResult) {
        showStartupError(generatedResult.error().message);
        return 1;
    }

    MainWindow mainWindow(importService, analysisService);
    mainWindow.show();

    return application.exec();
}
