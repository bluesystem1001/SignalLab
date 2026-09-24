#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QListView>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "gui/main_window.hpp"
#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/core/signal_generator.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/database/sqlite_task_repository.hpp"
#include "signallab/io/filesystem_file_repository.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "signallab/task/scheduler.hpp"

namespace {

using namespace std::chrono_literals;

class TemporaryProject {
public:
    explicit TemporaryProject(const std::string& purpose)
    {
        const auto uniqueNumber =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-stage5-gui-" + purpose + "-"
               + std::to_string(uniqueNumber));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryProject()
    {
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    TemporaryProject(const TemporaryProject&) = delete;
    TemporaryProject& operator=(const TemporaryProject&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return root_ / "runtime.sqlite3";
    }

private:
    std::filesystem::path root_;
};

std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

signallab::core::Signal makeLargeSignal(std::size_t sampleCount)
{
    constexpr double sampleRate = 48'000.0;
    signallab::core::SignalMetadata metadata{
        "Stage 5 large signal", "amplitude", "GUI runtime test"};
    auto signal = signallab::core::generateSineWave(
        1'000.0,
        1.0,
        sampleRate,
        static_cast<double>(sampleCount) / sampleRate,
        std::move(metadata));
    return std::move(signal).value();
}

signallab::repository::StoredSignal saveLargeSignal(
    signallab::database::DatabaseExecutor& database,
    std::size_t sampleCount,
    std::string sourceKey = "stage5-large-signal")
{
    auto stored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            std::move(sourceKey),
            {},
            "{}",
            std::nullopt,
            makeLargeSignal(sampleCount)});
    return std::move(stored).value();
}

bool allTerminal(const signallab::task::Scheduler& scheduler)
{
    const auto snapshots = scheduler.snapshots();
    return !snapshots.empty()
        && std::all_of(
            snapshots.begin(),
            snapshots.end(),
            [](const signallab::task::TaskSnapshot& snapshot) {
                return signallab::task::isTerminal(snapshot.status);
            });
}

TEST(Stage5GuiRuntimeTest, ThreeLargeAnalysesKeepTheQtEventLoopResponsive)
{
    TemporaryProject project("responsive");
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());
    const auto stored = saveLargeSignal(database, std::size_t{1} << 20U);
    static_cast<void>(saveLargeSignal(
        database,
        std::size_t{1} << 12U,
        "stage5-secondary-signal"));

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);

    signallab::task::SchedulerConfig schedulerConfig;
    schedulerConfig.computeThreadCount = 3;
    schedulerConfig.ioThreadCount = 1;
    signallab::task::Scheduler scheduler(schedulerConfig);
    signallab::app::BackgroundAnalysisService backgroundAnalysis(
        analysis, scheduler);
    signallab::app::BackgroundFileService backgroundFiles(
        imports, scheduler);
    MainWindow window(
        imports,
        analysis,
        backgroundFiles,
        backgroundAnalysis,
        scheduler);
    window.show();

    auto* signalList = window.findChild<QListView*>("signalListView");
    ASSERT_NE(signalList, nullptr);
    ASSERT_NE(signalList->model(), nullptr);
    ASSERT_EQ(signalList->model()->rowCount(), 2);

    for (int index = 0; index < 3; ++index) {
        const auto submitted = backgroundAnalysis.submit(stored.recordId);
        ASSERT_TRUE(submitted) << submitted.error().message;
    }

    int heartbeatCount = 0;
    int signalSwitchCount = 0;
    qint64 maximumHeartbeatGapMs = 0;
    qint64 previousHeartbeatMs = 0;
    bool timedOut = false;
    QEventLoop loop;
    QElapsedTimer eventTimer;
    eventTimer.start();
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
        const qint64 now = eventTimer.elapsed();
        if (previousHeartbeatMs != 0) {
            maximumHeartbeatGapMs = std::max(
                maximumHeartbeatGapMs,
                now - previousHeartbeatMs);
        }
        previousHeartbeatMs = now;
        ++heartbeatCount;
    });
    heartbeat.start(5);

    QTimer interactionTimer;
    QObject::connect(&interactionTimer, &QTimer::timeout, [&]() {
        const int currentRow = signalList->currentIndex().row();
        const int nextRow = currentRow == 0 ? 1 : 0;
        signalList->setCurrentIndex(signalList->model()->index(nextRow, 0));
        ++signalSwitchCount;
        const int widthDelta = signalSwitchCount % 2 == 0 ? -1 : 1;
        window.resize(window.width() + widthDelta, window.height());
    });
    interactionTimer.start(25);

    QTimer completionPoll;
    QObject::connect(&completionPoll, &QTimer::timeout, [&]() {
        if (allTerminal(scheduler)) {
            loop.quit();
        }
    });
    completionPoll.start(5);
    QTimer::singleShot(10'000, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    loop.exec();
    heartbeat.stop();
    interactionTimer.stop();
    completionPoll.stop();
    QCoreApplication::processEvents();

    EXPECT_FALSE(timedOut);
    EXPECT_GE(heartbeatCount, 5);
    EXPECT_GE(signalSwitchCount, 2);
    EXPECT_LT(maximumHeartbeatGapMs, 200);
    for (const auto& snapshot : scheduler.snapshots()) {
        EXPECT_EQ(snapshot.status, signallab::task::TaskStatus::Succeeded);
    }

    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
    window.close();
}

TEST(Stage5GuiRuntimeTest, AppShutdownOrderCancelsJoinsAndFlushesRunningWork)
{
    TemporaryProject project("shutdown");
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());
    const auto stored = saveLargeSignal(database, std::size_t{1} << 20U);

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);

    signallab::task::SchedulerConfig schedulerConfig;
    schedulerConfig.computeThreadCount = 3;
    schedulerConfig.ioThreadCount = 1;
    signallab::task::Scheduler scheduler(schedulerConfig);
    signallab::app::BackgroundAnalysisService backgroundAnalysis(
        analysis, scheduler);
    signallab::app::BackgroundFileService backgroundFiles(
        imports, scheduler);
    MainWindow window(
        imports,
        analysis,
        backgroundFiles,
        backgroundAnalysis,
        scheduler);
    window.show();

    for (int index = 0; index < 3; ++index) {
        const auto submitted = backgroundAnalysis.submit(stored.recordId);
        ASSERT_TRUE(submitted) << submitted.error().message;
    }

    QEventLoop startLoop;
    bool startTimedOut = false;
    std::string startError;
    QTimer startPoll;
    QObject::connect(&startPoll, &QTimer::timeout, [&]() {
        const auto history = database.listTasks(20);
        if (!history) {
            startError = history.error().message;
            startLoop.quit();
            return;
        }
        const bool hasPersistentRunningTask = std::any_of(
            history.value().begin(),
            history.value().end(),
            [](const signallab::repository::AnalysisTaskRecord& task) {
                return task.status
                    == signallab::repository::AnalysisTaskStatus::Running;
            });
        if (hasPersistentRunningTask) {
            startLoop.quit();
        }
    });
    startPoll.start(1);
    QTimer::singleShot(2'000, &startLoop, [&]() {
        startTimedOut = true;
        startLoop.quit();
    });
    startLoop.exec();
    startPoll.stop();
    ASSERT_TRUE(startError.empty()) << startError;
    ASSERT_FALSE(startTimedOut);

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    QElapsedTimer shutdownDuration;
    shutdownDuration.start();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
    EXPECT_LT(shutdownDuration.elapsed(), 5'000);
    EXPECT_TRUE(allTerminal(scheduler));

    signallab::database::DatabaseConnection verification(
        project.databasePath());
    ASSERT_TRUE(verification.open());
    signallab::database::SqliteTaskRepository tasks(verification);
    const auto history = tasks.listTasks(20);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_FALSE(history.value().empty());
    for (const auto& task : history.value()) {
        EXPECT_NE(
            task.status,
            signallab::repository::AnalysisTaskStatus::Pending);
        EXPECT_NE(
            task.status,
            signallab::repository::AnalysisTaskStatus::Running);
        if (task.status
            == signallab::repository::AnalysisTaskStatus::Cancelled) {
            const auto features = tasks.listFeatureResults(task.id);
            ASSERT_TRUE(features) << features.error().message;
            EXPECT_TRUE(features.value().empty());
        }
    }
}

}  // namespace
