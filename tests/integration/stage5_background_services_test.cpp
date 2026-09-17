#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/core/signal_generator.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/repository/file_repository.hpp"
#include "signallab/repository/task_repository.hpp"
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
            / ("signallab-stage5-background-" + purpose + "-"
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
        return root_ / "background.sqlite3";
    }

private:
    std::filesystem::path root_;
};

std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

signallab::core::Signal makeAnalysisSignal()
{
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t sampleCount = 65'536;
    signallab::core::SignalMetadata metadata{
        "Background test signal", "amplitude", "integration test"};
    auto signal = signallab::core::generateSineWave(
        1'000.0,
        1.0,
        sampleRate,
        static_cast<double>(sampleCount) / sampleRate,
        std::move(metadata));
    return std::move(signal).value();
}

signallab::repository::StoredSignal saveAnalysisSignal(
    signallab::database::DatabaseExecutor& database,
    std::string sourceKey)
{
    auto stored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            std::move(sourceKey),
            {},
            "{}",
            std::nullopt,
            makeAnalysisSignal()});
    return std::move(stored).value();
}

signallab::task::SchedulerConfig schedulerConfig(
    std::size_t computeThreads)
{
    signallab::task::SchedulerConfig config;
    config.computeThreadCount = computeThreads;
    config.ioThreadCount = 1;
    config.progressInterval = 0ms;
    return config;
}

template<typename Service, typename Event>
std::vector<Event> waitForTerminalEvents(
    Service& service,
    std::size_t expectedCount,
    std::chrono::milliseconds timeout = 5s)
{
    std::vector<Event> terminalEvents;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (terminalEvents.size() < expectedCount
           && std::chrono::steady_clock::now() < deadline) {
        auto events = service.takeEvents();
        for (auto& event : events) {
            if (signallab::task::isTerminal(event.task.status)) {
                terminalEvents.push_back(std::move(event));
            }
        }
        if (terminalEvents.size() < expectedCount) {
            std::this_thread::sleep_for(1ms);
        }
    }
    return terminalEvents;
}

class FakeFileRepository final
    : public signallab::repository::FileRepository {
public:
    void failCsvPreview(bool fail) noexcept
    {
        failCsvPreview_ = fail;
    }

    [[nodiscard]] std::thread::id importThread() const
    {
        std::lock_guard lock(mutex_);
        return importThread_;
    }

    signallab::core::Result<signallab::repository::ImportPreview>
    previewCsv(
        const std::filesystem::path& sourcePath,
        const signallab::repository::CsvImportOptions&,
        std::stop_token stopToken) const override
    {
        if (stopToken.stop_requested()) {
            return cancelledPreview();
        }
        if (failCsvPreview_) {
            return signallab::core::Result<
                signallab::repository::ImportPreview>::failure(
                signallab::core::ErrorCode::FileOpenFailed,
                "Injected CSV preview failure.");
        }
        return signallab::core::Result<
            signallab::repository::ImportPreview>::success(
            signallab::repository::ImportPreview{
                sourcePath,
                signallab::repository::ImportFormat::Csv,
                4,
                2,
                0,
                4.0,
                {"time", "value"},
                {{"0", "0"}, {"0.25", "1"}}});
    }

    signallab::core::Result<signallab::repository::ImportPreview>
    previewWav(
        const std::filesystem::path& sourcePath,
        std::stop_token stopToken) const override
    {
        if (stopToken.stop_requested()) {
            return cancelledPreview();
        }
        return signallab::core::Result<
            signallab::repository::ImportPreview>::success(
            signallab::repository::ImportPreview{
                sourcePath,
                signallab::repository::ImportFormat::Wav,
                4,
                0,
                1,
                4.0,
                {},
                {}});
    }

    signallab::core::Result<signallab::repository::FileImport>
    importCsv(
        const std::filesystem::path& sourcePath,
        const signallab::repository::CsvImportOptions& options,
        std::stop_token stopToken) override
    {
        {
            std::lock_guard lock(mutex_);
            importThread_ = std::this_thread::get_id();
        }
        return makeImport(
            sourcePath,
            signallab::repository::SignalSourceType::Csv,
            options.metadata,
            stopToken);
    }

    signallab::core::Result<signallab::repository::FileImport>
    importWav(
        const std::filesystem::path& sourcePath,
        const signallab::repository::WavImportOptions& options,
        std::stop_token stopToken) override
    {
        {
            std::lock_guard lock(mutex_);
            importThread_ = std::this_thread::get_id();
        }
        return makeImport(
            sourcePath,
            signallab::repository::SignalSourceType::Wav,
            options.metadata,
            stopToken);
    }

    signallab::core::Result<bool> validateStoredFile(
        const std::filesystem::path&,
        const std::string&,
        std::stop_token stopToken) const override
    {
        if (stopToken.stop_requested()) {
            return signallab::core::Result<bool>::failure(
                signallab::core::ErrorCode::Cancelled,
                "Stored-file validation was cancelled.");
        }
        return signallab::core::Result<bool>::success(true);
    }

    signallab::core::Result<bool> removeStoredFile(
        const std::filesystem::path&,
        std::stop_token) override
    {
        return signallab::core::Result<bool>::success(true);
    }

private:
    static signallab::core::Result<
        signallab::repository::ImportPreview> cancelledPreview()
    {
        return signallab::core::Result<
            signallab::repository::ImportPreview>::failure(
            signallab::core::ErrorCode::Cancelled,
            "File preview was cancelled.");
    }

    static signallab::core::Result<signallab::repository::FileImport>
    makeImport(
        const std::filesystem::path& sourcePath,
        signallab::repository::SignalSourceType sourceType,
        const signallab::core::SignalMetadata& metadata,
        std::stop_token stopToken)
    {
        if (stopToken.stop_requested()) {
            return signallab::core::Result<
                signallab::repository::FileImport>::failure(
                signallab::core::ErrorCode::Cancelled,
                "File import was cancelled.");
        }

        auto signal = signallab::core::Signal::create(
            {0.0, 1.0, 0.0, -1.0},
            4.0,
            metadata);
        return signallab::core::Result<
            signallab::repository::FileImport>::success(
            signallab::repository::FileImport{
                sourceType,
                std::string(64, 'a'),
                sourcePath,
                std::filesystem::path("data/imports/fake.csv"),
                32,
                false,
                std::move(signal).value()});
    }

    mutable std::mutex mutex_;
    std::thread::id importThread_;
    bool failCsvPreview_{false};
};

// The gate is deliberately placed after the database RUNNING transition.
// It makes cancellation deterministic without slowing down production code.
class GatedTaskRepository final
    : public signallab::repository::TaskRepository {
public:
    explicit GatedTaskRepository(
        signallab::repository::TaskRepository& delegate,
        std::size_t gatedStartCount)
        : delegate_(delegate),
          gatedStartCount_(gatedStartCount)
    {
    }

    ~GatedTaskRepository() override
    {
        release();
    }

    [[nodiscard]] bool waitUntilAllGated(
        std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this]() {
            return enteredGate_ >= gatedStartCount_;
        });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    signallab::core::Result<std::int64_t> createTask(
        const signallab::repository::AnalysisTaskStart& task) override
    {
        return delegate_.createTask(task);
    }

    signallab::core::Result<bool> startTask(
        std::int64_t taskId) override
    {
        auto started = delegate_.startTask(taskId);
        if (!started || !started.value()) {
            return started;
        }

        std::unique_lock lock(mutex_);
        if (enteredGate_ < gatedStartCount_) {
            ++enteredGate_;
            condition_.notify_all();
            condition_.wait(lock, [this]() { return released_; });
        }
        return started;
    }

    signallab::core::Result<std::string> completeTask(
        std::int64_t taskId,
        const std::vector<signallab::repository::FeatureValue>& features)
        override
    {
        return delegate_.completeTask(taskId, features);
    }

    signallab::core::Result<bool> failTask(
        std::int64_t taskId,
        const std::string& errorMessage) override
    {
        return delegate_.failTask(taskId, errorMessage);
    }

    signallab::core::Result<bool> cancelTask(
        std::int64_t taskId,
        const std::string& reason) override
    {
        return delegate_.cancelTask(taskId, reason);
    }

    signallab::core::Result<std::size_t> interruptRunningTasks() override
    {
        return delegate_.interruptRunningTasks();
    }

    signallab::core::Result<
        std::vector<signallab::repository::AnalysisTaskRecord>>
    listTasks(std::size_t maximumCount) override
    {
        return delegate_.listTasks(maximumCount);
    }

    signallab::core::Result<
        std::vector<signallab::repository::FeatureResultRecord>>
    listFeatureResults(std::int64_t taskId) override
    {
        return delegate_.listFeatureResults(taskId);
    }

private:
    signallab::repository::TaskRepository& delegate_;
    const std::size_t gatedStartCount_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t enteredGate_{0};
    bool released_{false};
};

class GateReleaseGuard {
public:
    explicit GateReleaseGuard(GatedTaskRepository& repository)
        : repository_(repository)
    {
    }

    ~GateReleaseGuard()
    {
        repository_.release();
    }

    GateReleaseGuard(const GateReleaseGuard&) = delete;
    GateReleaseGuard& operator=(const GateReleaseGuard&) = delete;

private:
    GatedTaskRepository& repository_;
};

TEST(AnalysisServiceStageFiveTest,
     ReportsProgressThroughThePersistentCommit)
{
    TemporaryProject project("progress");
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());
    const auto stored = saveAnalysisSignal(database, "progress-signal");
    signallab::app::AnalysisService analysis(database, database);

    std::vector<int> percentages;
    std::vector<std::string> messages;
    const auto result = analysis.analyze(
        stored.recordId,
        {},
        {},
        [&percentages, &messages](int percent, std::string message) {
            percentages.push_back(percent);
            messages.push_back(std::move(message));
        });

    ASSERT_TRUE(result) << result.error().message;
    ASSERT_FALSE(percentages.empty());
    EXPECT_EQ(percentages.front(), 0);
    EXPECT_EQ(percentages.back(), 100);
    EXPECT_TRUE(std::is_sorted(percentages.begin(), percentages.end()));
    EXPECT_TRUE(std::all_of(
        messages.begin(),
        messages.end(),
        [](const std::string& message) { return !message.empty(); }));
    EXPECT_EQ(messages.back(), "Analysis saved");

    const auto history = database.listTasks(10);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 1U);
    EXPECT_EQ(
        history.value().front().status,
        signallab::repository::AnalysisTaskStatus::Succeeded);
    ASSERT_TRUE(database.shutdown());
}

TEST(BackgroundFileServiceTest,
     PublishesSuccessAndFailureWithoutMutatingTheUiSignalList)
{
    TemporaryProject project("file-events");
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());

    FakeFileRepository files;
    signallab::app::ImportService imports(files, database);
    signallab::task::Scheduler scheduler(schedulerConfig(1));
    signallab::app::BackgroundFileService background(imports, scheduler);

    signallab::repository::CsvImportOptions options;
    options.metadata = {"Imported in background", "V", "fake.csv"};
    const std::thread::id uiThread = std::this_thread::get_id();
    const auto submitted = background.importCsv("fake.csv", options);
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 5s));

    auto successEvents = waitForTerminalEvents<
        signallab::app::BackgroundFileService,
        signallab::app::BackgroundFileEvent>(background, 1);
    ASSERT_EQ(successEvents.size(), 1U);
    auto& success = successEvents.front();
    EXPECT_EQ(success.task.status, signallab::task::TaskStatus::Succeeded);
    EXPECT_EQ(
        success.operation,
        signallab::app::BackgroundFileOperation::ImportCsv);
    EXPECT_FALSE(success.preview);
    ASSERT_TRUE(success.importedSignal);
    EXPECT_TRUE(imports.entries().empty());
    EXPECT_NE(files.importThread(), uiThread);

    const auto acceptedIndex = imports.acceptImportedSignal(
        std::move(*success.importedSignal));
    EXPECT_EQ(acceptedIndex, 0U);
    ASSERT_EQ(imports.entries().size(), 1U);
    EXPECT_EQ(imports.entries().front().displayName, "Imported in background");

    files.failCsvPreview(true);
    const auto failedSubmission = background.previewCsv("broken.csv", options);
    ASSERT_TRUE(failedSubmission) << failedSubmission.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(failedSubmission.value(), 5s));
    auto failureEvents = waitForTerminalEvents<
        signallab::app::BackgroundFileService,
        signallab::app::BackgroundFileEvent>(background, 1);
    ASSERT_EQ(failureEvents.size(), 1U);
    const auto& failure = failureEvents.front();
    EXPECT_EQ(failure.task.status, signallab::task::TaskStatus::Failed);
    EXPECT_NE(
        failure.task.errorMessage.find("Injected CSV preview failure"),
        std::string::npos);
    EXPECT_FALSE(failure.preview);
    EXPECT_FALSE(failure.importedSignal);
    EXPECT_EQ(imports.entries().size(), 1U);

    background.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

TEST(BackgroundAnalysisServiceTest,
     TenTasksCancelThreeWithoutFeaturesAndCompleteTheOtherSeven)
{
    TemporaryProject project("ten-tasks");
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());
    const auto stored = saveAnalysisSignal(database, "ten-task-signal");

    GatedTaskRepository gatedTasks(database, 3);
    signallab::app::AnalysisService analysis(gatedTasks, database);
    signallab::task::Scheduler scheduler(schedulerConfig(3));
    signallab::app::BackgroundAnalysisService background(
        analysis, scheduler);
    GateReleaseGuard gateGuard(gatedTasks);

    std::vector<signallab::task::TaskId> submittedIds;
    for (int index = 0; index < 10; ++index) {
        auto submitted = background.submit(stored.recordId);
        ASSERT_TRUE(submitted) << submitted.error().message;
        submittedIds.push_back(submitted.value());
    }

    ASSERT_TRUE(gatedTasks.waitUntilAllGated(5s));
    const auto gatedSnapshots = scheduler.snapshots();
    std::set<signallab::task::TaskId> cancelledIds;
    for (const auto& snapshot : gatedSnapshots) {
        if (snapshot.status == signallab::task::TaskStatus::Running) {
            cancelledIds.insert(snapshot.id);
        }
    }
    ASSERT_EQ(cancelledIds.size(), 3U);
    for (const auto id : cancelledIds) {
        EXPECT_TRUE(scheduler.cancel(id));
    }
    gatedTasks.release();

    ASSERT_TRUE(scheduler.waitForIdle(30s));
    auto terminalEvents = waitForTerminalEvents<
        signallab::app::BackgroundAnalysisService,
        signallab::app::BackgroundAnalysisEvent>(background, 10, 10s);
    ASSERT_EQ(terminalEvents.size(), 10U);

    std::size_t cancelledEventCount = 0;
    std::size_t succeededEventCount = 0;
    for (const auto& event : terminalEvents) {
        EXPECT_EQ(event.signalRecordId, stored.recordId);
        if (cancelledIds.contains(event.task.id)) {
            ++cancelledEventCount;
            EXPECT_EQ(
                event.task.status,
                signallab::task::TaskStatus::Cancelled);
            EXPECT_FALSE(event.result);
        } else {
            ++succeededEventCount;
            EXPECT_EQ(
                event.task.status,
                signallab::task::TaskStatus::Succeeded);
            ASSERT_TRUE(event.result);
            EXPECT_FALSE(event.result->spectrum.amplitudes.empty());
        }
    }
    EXPECT_EQ(cancelledEventCount, 3U);
    EXPECT_EQ(succeededEventCount, 7U);

    const auto history = database.listTasks(20);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 10U);
    std::size_t cancelledHistoryCount = 0;
    std::size_t succeededHistoryCount = 0;
    for (const auto& task : history.value()) {
        const auto features = database.listFeatureResults(task.id);
        ASSERT_TRUE(features) << features.error().message;
        if (task.status
            == signallab::repository::AnalysisTaskStatus::Cancelled) {
            ++cancelledHistoryCount;
            EXPECT_TRUE(features.value().empty());
        } else if (task.status
                   == signallab::repository::AnalysisTaskStatus::Succeeded) {
            ++succeededHistoryCount;
            EXPECT_FALSE(features.value().empty());
        } else {
            ADD_FAILURE() << "Unexpected persistent task state: "
                          << static_cast<int>(task.status);
        }
    }
    EXPECT_EQ(cancelledHistoryCount, 3U);
    EXPECT_EQ(succeededHistoryCount, 7U);

    background.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

}  // namespace
