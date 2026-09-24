#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "signallab/app/background_script_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/app/script_service.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/io/filesystem_file_repository.hpp"
#include "signallab/io/filesystem_script_file_gateway.hpp"
#include "signallab/task/scheduler.hpp"

namespace {

using namespace std::chrono_literals;

std::atomic<unsigned long long> temporaryProjectCounter{0};

class TemporaryProject {
public:
    explicit TemporaryProject(const std::string& purpose)
    {
        const auto clockValue =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto counterValue = temporaryProjectCounter.fetch_add(
            1,
            std::memory_order_relaxed);
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-stage7-background-script-" + purpose + "-"
               + std::to_string(clockValue) + "-"
               + std::to_string(counterValue));
        std::filesystem::create_directories(inputRoot());
        std::filesystem::create_directories(outputRoot());
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

    [[nodiscard]] std::filesystem::path inputRoot() const
    {
        return root_ / "input";
    }

    [[nodiscard]] std::filesystem::path outputRoot() const
    {
        return root_ / "output";
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return root_ / "stage7.sqlite3";
    }

private:
    std::filesystem::path root_;
};

std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

signallab::task::SchedulerConfig schedulerConfig()
{
    signallab::task::SchedulerConfig config;
    config.computeThreadCount = 1;
    config.ioThreadCount = 1;
    config.progressInterval = 0ms;
    return config;
}

std::optional<signallab::app::BackgroundScriptEvent> waitForTerminalEvent(
    signallab::app::BackgroundScriptService& service,
    signallab::task::TaskId taskId,
    std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto events = service.takeEvents();
        for (auto& event : events) {
            if (event.task.id == taskId
                && signallab::task::isTerminal(event.task.status)) {
                return std::move(event);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

class BackgroundScriptServiceTest : public ::testing::Test {
protected:
    explicit BackgroundScriptServiceTest(std::string purpose = "service")
        : project_(std::move(purpose)),
          database_(project_.databasePath(), migrationsDirectory()),
          files_(project_.root()),
          imports_(files_, database_),
          gateway_(project_.inputRoot(), project_.outputRoot()),
          scripts_(imports_, database_, gateway_),
          scheduler_(schedulerConfig()),
          background_(scripts_, scheduler_)
    {
    }

    void SetUp() override
    {
        const auto started = database_.start();
        ASSERT_TRUE(started) << started.error().message;
    }

    void TearDown() override
    {
        background_.clearEventNotifier();
        scheduler_.shutdownAndWait();
        if (database_.isRunning()) {
            const auto stopped = database_.shutdown();
            EXPECT_TRUE(stopped) << stopped.error().message;
        }
    }

    TemporaryProject project_;
    signallab::database::DatabaseExecutor database_;
    signallab::io::FilesystemFileRepository files_;
    signallab::app::ImportService imports_;
    signallab::io::FilesystemScriptFileGateway gateway_;
    signallab::app::ScriptService scripts_;
    signallab::task::Scheduler scheduler_;
    signallab::app::BackgroundScriptService background_;
};

TEST_F(BackgroundScriptServiceTest,
       DeliversSuccessfulExecutionOnTheComputeLane)
{
    const std::string source = "let answer = 42;\n";
    auto submitted = background_.submit(source);
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(submitted.value(), 5s));

    auto event = waitForTerminalEvent(background_, submitted.value());
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(
        event->operation,
        signallab::app::BackgroundScriptOperation::Execute);
    EXPECT_EQ(event->task.lane, signallab::task::TaskLane::Compute);
    EXPECT_EQ(event->task.status, signallab::task::TaskStatus::Succeeded);
    ASSERT_TRUE(event->source);
    EXPECT_EQ(*event->source, source);
    ASSERT_TRUE(event->result);
    EXPECT_EQ(
        event->result->status,
        signallab::app::ScriptExecutionStatus::Succeeded);
    EXPECT_FALSE(event->history);
}

TEST_F(BackgroundScriptServiceTest,
       DeliversDiagnosticsWhenExecutionFails)
{
    const std::string source =
        "let prefix = 1;\n"
        "value = fft(prefix;\n";
    auto submitted = background_.submit(source);
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(submitted.value(), 5s));

    auto event = waitForTerminalEvent(background_, submitted.value());
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->task.status, signallab::task::TaskStatus::Failed);
    EXPECT_NE(event->task.errorMessage.find("PAR021"), std::string::npos);
    ASSERT_TRUE(event->result);
    EXPECT_EQ(
        event->result->status,
        signallab::app::ScriptExecutionStatus::Failed);
    ASSERT_FALSE(event->result->diagnostics.empty());
    EXPECT_EQ(event->result->diagnostics.front().code, "PAR021");
    EXPECT_EQ(event->result->diagnostics.front().span.begin.line, 2U);
}

TEST_F(BackgroundScriptServiceTest,
       PreservesTheSubmittedSourceAcrossMutationAndRetry)
{
    const std::string expected = "value = fft(1;\n";
    std::string source = expected;
    auto submitted = background_.submit(source);
    ASSERT_TRUE(submitted) << submitted.error().message;
    source.assign("let changed = true;\n");
    ASSERT_TRUE(scheduler_.waitForTerminal(submitted.value(), 5s));

    auto first = waitForTerminalEvent(background_, submitted.value());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->source);
    ASSERT_TRUE(first->result);
    EXPECT_EQ(*first->source, expected);

    auto retried = scheduler_.retry(submitted.value());
    ASSERT_TRUE(retried) << retried.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(retried.value(), 5s));
    auto second = waitForTerminalEvent(background_, retried.value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->source);
    ASSERT_TRUE(second->result);
    EXPECT_EQ(*second->source, expected);
    EXPECT_EQ(second->source, first->source);
    EXPECT_NE(second->result->historyId, first->result->historyId);
    EXPECT_EQ(second->task.retryOf, submitted.value());
}

TEST_F(BackgroundScriptServiceTest,
       MapsCooperativeExecutionCancellationAndDeliversItsResult)
{
    std::string source;
    source.reserve(600'000U);
    for (std::size_t index = 0; index < 200'000U; ++index) {
        source += "1;\n";
    }

    auto submitted = background_.submit(std::move(source));
    ASSERT_TRUE(submitted) << submitted.error().message;

    const auto runningDeadline = std::chrono::steady_clock::now() + 5s;
    bool observedRunning = false;
    while (std::chrono::steady_clock::now() < runningDeadline) {
        const auto snapshot = scheduler_.snapshot(submitted.value());
        ASSERT_TRUE(snapshot.has_value());
        if (snapshot->status == signallab::task::TaskStatus::Running) {
            observedRunning = true;
            break;
        }
        ASSERT_FALSE(signallab::task::isTerminal(snapshot->status));
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(observedRunning);
    ASSERT_TRUE(scheduler_.cancel(submitted.value()));
    ASSERT_TRUE(scheduler_.waitForTerminal(submitted.value(), 10s));

    auto event = waitForTerminalEvent(
        background_,
        submitted.value(),
        10s);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->task.status, signallab::task::TaskStatus::Cancelled);
    ASSERT_TRUE(event->result);
    EXPECT_EQ(
        event->result->status,
        signallab::app::ScriptExecutionStatus::Cancelled);
    ASSERT_TRUE(event->result->runtimeIssue.has_value());
    EXPECT_EQ(event->result->runtimeIssue->code, "RUN001");
}

TEST_F(BackgroundScriptServiceTest,
       LoadsHistoryAsynchronouslyOnTheIoLane)
{
    auto executed = background_.submit("let answer = 42;\n");
    ASSERT_TRUE(executed) << executed.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(executed.value(), 5s));
    ASSERT_TRUE(waitForTerminalEvent(background_, executed.value()));

    auto requested = background_.requestHistory(1);
    ASSERT_TRUE(requested) << requested.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(requested.value(), 5s));
    auto event = waitForTerminalEvent(background_, requested.value());

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(
        event->operation,
        signallab::app::BackgroundScriptOperation::History);
    EXPECT_EQ(event->task.lane, signallab::task::TaskLane::Io);
    EXPECT_EQ(event->task.status, signallab::task::TaskStatus::Succeeded);
    EXPECT_FALSE(event->source);
    EXPECT_FALSE(event->result);
    ASSERT_TRUE(event->history);
    ASSERT_EQ(event->history->size(), 1U);
    EXPECT_EQ(
        event->history->front().status,
        signallab::repository::ScriptHistoryStatus::Succeeded);
    EXPECT_EQ(event->history->front().scriptText, "let answer = 42;\n");

    auto retried = scheduler_.retry(requested.value());
    ASSERT_TRUE(retried) << retried.error().message;
    ASSERT_TRUE(scheduler_.waitForTerminal(retried.value(), 5s));
    auto retryEvent = waitForTerminalEvent(background_, retried.value());
    ASSERT_TRUE(retryEvent.has_value());
    EXPECT_EQ(retryEvent->task.retryOf, requested.value());
    EXPECT_EQ(retryEvent->task.lane, signallab::task::TaskLane::Io);
    ASSERT_TRUE(retryEvent->history);
    ASSERT_EQ(retryEvent->history->size(), 1U);
    EXPECT_EQ(
        retryEvent->history->front().id,
        event->history->front().id);
}

}  // namespace
