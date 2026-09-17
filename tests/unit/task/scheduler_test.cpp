#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "signallab/core/error.hpp"
#include "signallab/task/scheduler.hpp"

namespace signallab::task {
namespace {

using namespace std::chrono_literals;

core::Result<bool> cancelledResult()
{
    return core::Result<bool>::failure(
        core::ErrorCode::Cancelled,
        "Cancellation was observed by the task.");
}

SchedulerConfig testConfig(std::size_t computeThreads)
{
    SchedulerConfig config;
    config.computeThreadCount = computeThreads;
    config.ioThreadCount = 1;
    config.progressInterval = 0ms;
    return config;
}

TEST(SchedulerTest, DefaultWorkerFormulaIsSafeForSmallMachines)
{
    EXPECT_EQ(calculateDefaultComputeThreadCount(0), 2U);
    EXPECT_EQ(calculateDefaultComputeThreadCount(1), 2U);
    EXPECT_EQ(calculateDefaultComputeThreadCount(2), 2U);
    EXPECT_EQ(calculateDefaultComputeThreadCount(3), 2U);
    EXPECT_EQ(calculateDefaultComputeThreadCount(8), 6U);
}

TEST(SchedulerTest, IoPoolConfigurationAllowsOnlyOneOrTwoWorkers)
{
    SchedulerConfig twoIoWorkers = testConfig(1);
    twoIoWorkers.ioThreadCount = 2;
    Scheduler validScheduler(twoIoWorkers);
    EXPECT_EQ(validScheduler.ioThreadCount(), 2U);

    SchedulerConfig tooManyIoWorkers = testConfig(1);
    tooManyIoWorkers.ioThreadCount = 3;
    EXPECT_THROW(
        static_cast<void>(Scheduler(tooManyIoWorkers)),
        std::invalid_argument);
}

TEST(SchedulerTest, TenTasksCanCancelTwoRunningAndOnePending)
{
    Scheduler scheduler(testConfig(3));
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    int blockingTasksStarted = 0;
    bool release = false;
    std::vector<TaskId> ids;

    auto action = [&](const TaskContext& context) {
        if (context.id() <= 3) {
            std::unique_lock lock(gateMutex);
            ++blockingTasksStarted;
            gateChanged.notify_all();
            while (!release && !context.stopRequested()) {
                gateChanged.wait_for(lock, 1ms);
            }
        }

        if (context.stopRequested()) {
            return cancelledResult();
        }
        return core::Result<bool>::success(true);
    };

    for (int index = 0; index < 3; ++index) {
        auto submitted = scheduler.submit(TaskSpec{"analysis", TaskLane::Compute, action, {}});
        ASSERT_TRUE(submitted) << submitted.error().message;
        ids.push_back(submitted.value());
    }

    {
        std::unique_lock lock(gateMutex);
        ASSERT_TRUE(gateChanged.wait_for(lock, 1s, [&]() {
            return blockingTasksStarted == 3;
        }));
    }

    for (int index = 3; index < 10; ++index) {
        auto submitted = scheduler.submit(TaskSpec{"analysis", TaskLane::Compute, action, {}});
        ASSERT_TRUE(submitted) << submitted.error().message;
        ids.push_back(submitted.value());
    }

    // The last task is certainly queued because all three workers are gated.
    EXPECT_TRUE(scheduler.cancel(ids[9]));
    EXPECT_TRUE(scheduler.cancel(ids[0]));
    EXPECT_TRUE(scheduler.cancel(ids[1]));

    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateChanged.notify_all();

    ASSERT_TRUE(scheduler.waitForIdle(2s));
    int succeeded = 0;
    int cancelled = 0;
    for (const auto& snapshot : scheduler.snapshots()) {
        if (snapshot.status == TaskStatus::Succeeded) {
            ++succeeded;
        } else if (snapshot.status == TaskStatus::Cancelled) {
            ++cancelled;
        }
        EXPECT_TRUE(isTerminal(snapshot.status));
    }
    EXPECT_EQ(succeeded, 7);
    EXPECT_EQ(cancelled, 3);
}

TEST(SchedulerTest, RunningTaskObservesCancellationWithinTwoHundredMilliseconds)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::promise<std::chrono::steady_clock::time_point> cancelledPromise;
    auto cancelled = cancelledPromise.get_future();
    std::atomic<bool> cancellationReported{false};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Cancelled
            && !cancellationReported.exchange(true)) {
            cancelledPromise.set_value(std::chrono::steady_clock::now());
        }
    };
    TaskAction action = [&](const TaskContext& context) {
        startedPromise.set_value();
        while (!context.stopRequested()) {
            std::this_thread::sleep_for(1ms);
        }
        return cancelledResult();
    };

    auto submitted = scheduler.submit(TaskSpec{
        "cancel-latency", TaskLane::Compute, action, observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);

    const auto requestedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(scheduler.cancel(submitted.value()));
    ASSERT_EQ(cancelled.wait_for(1s), std::future_status::ready);
    const auto observedAt = cancelled.get();

    EXPECT_LT(observedAt - requestedAt, 200ms);
}

TEST(SchedulerTest, RetryCreatesANewIdAfterAnException)
{
    Scheduler scheduler(testConfig(1));
    auto attempts = std::make_shared<std::atomic<int>>(0);
    TaskAction action = [attempts](const TaskContext&) {
        if (attempts->fetch_add(1) == 0) {
            throw std::runtime_error("injected failure");
        }
        return core::Result<bool>::success(true);
    };

    auto first = scheduler.submit(TaskSpec{
        "retryable", TaskLane::Compute, action, {}});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(first.value(), 1s));
    const auto firstSnapshot = scheduler.snapshot(first.value());
    ASSERT_TRUE(firstSnapshot.has_value());
    EXPECT_EQ(firstSnapshot->status, TaskStatus::Failed);
    EXPECT_NE(firstSnapshot->errorMessage.find("injected failure"), std::string::npos);

    auto retried = scheduler.retry(first.value());
    ASSERT_TRUE(retried) << retried.error().message;
    EXPECT_NE(retried.value(), first.value());
    ASSERT_TRUE(scheduler.waitForTerminal(retried.value(), 1s));

    const auto retrySnapshot = scheduler.snapshot(retried.value());
    ASSERT_TRUE(retrySnapshot.has_value());
    EXPECT_EQ(retrySnapshot->status, TaskStatus::Succeeded);
    EXPECT_EQ(retrySnapshot->retryOf, first.value());
    EXPECT_EQ(attempts->load(), 2);
}

TEST(SchedulerTest, SuccessfulActionWinsAgainstALateCancellationRequest)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> actionStartedPromise;
    auto actionStarted = actionStartedPromise.get_future();
    std::promise<void> allowSuccessPromise;
    auto allowSuccess = allowSuccessPromise.get_future().share();

    TaskAction action = [&](const TaskContext&) {
        actionStartedPromise.set_value();
        allowSuccess.wait();
        // This represents an operation whose transaction already committed.
        return core::Result<bool>::success(true);
    };

    auto submitted = scheduler.submit(TaskSpec{
        "commit-wins", TaskLane::Compute, action, {}});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_EQ(actionStarted.wait_for(1s), std::future_status::ready);

    ASSERT_TRUE(scheduler.cancel(submitted.value()));
    allowSuccessPromise.set_value();
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));

    const auto finished = scheduler.snapshot(submitted.value());
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ(finished->status, TaskStatus::Succeeded);
}

TEST(SchedulerTest, ShutdownCancelsRunningAndPendingTasksAndIsIdempotent)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    TaskAction action = [&](const TaskContext& context) {
        startedPromise.set_value();
        while (!context.stopRequested()) {
            std::this_thread::sleep_for(1ms);
        }
        return cancelledResult();
    };

    auto running = scheduler.submit(TaskSpec{
        "running", TaskLane::Compute, action, {}});
    ASSERT_TRUE(running) << running.error().message;
    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);

    const auto earlyRetry = scheduler.retry(running.value());
    EXPECT_FALSE(earlyRetry);
    EXPECT_EQ(earlyRetry.error().code, core::ErrorCode::InvalidParameter);

    auto pending = scheduler.submit(TaskSpec{
        "pending",
        TaskLane::Compute,
        [](const TaskContext&) { return core::Result<bool>::success(true); },
        {}});
    ASSERT_TRUE(pending) << pending.error().message;

    scheduler.shutdownAndWait();
    scheduler.shutdownAndWait();

    ASSERT_TRUE(scheduler.waitForIdle(100ms));
    ASSERT_TRUE(scheduler.snapshot(running.value()).has_value());
    ASSERT_TRUE(scheduler.snapshot(pending.value()).has_value());
    EXPECT_EQ(
        scheduler.snapshot(running.value())->status,
        TaskStatus::Cancelled);
    EXPECT_EQ(
        scheduler.snapshot(pending.value())->status,
        TaskStatus::Cancelled);

    const auto rejected = scheduler.submit(TaskSpec{
        "late",
        TaskLane::Compute,
        [](const TaskContext&) { return core::Result<bool>::success(true); },
        {}});
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, core::ErrorCode::Cancelled);
}

TEST(SchedulerTest, ProgressObserverIsThrottledToAboutFiftyMilliseconds)
{
    SchedulerConfig config = testConfig(1);
    config.progressInterval = 50ms;
    Scheduler scheduler(config);
    std::mutex timesMutex;
    std::vector<std::chrono::steady_clock::time_point> progressTimes;

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Running
            && !snapshot.progressMessage.empty()) {
            std::lock_guard lock(timesMutex);
            progressTimes.push_back(std::chrono::steady_clock::now());
        }
    };
    TaskAction action = [](const TaskContext& context) {
        for (int percent = 1; percent <= 20; ++percent) {
            context.reportProgress(percent, "step");
            std::this_thread::sleep_for(5ms);
        }
        return core::Result<bool>::success(true);
    };

    auto submitted = scheduler.submit(TaskSpec{
        "progress", TaskLane::Compute, action, observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));

    std::lock_guard lock(timesMutex);
    ASSERT_GE(progressTimes.size(), 2U);
    // Windows may round each 5 ms sleep up to a longer timer tick, so the
    // total task duration is intentionally not used to predict an exact
    // event count. It still must be much lower than the 20 reports made.
    EXPECT_LT(progressTimes.size(), 20U);
    for (std::size_t index = 1; index < progressTimes.size(); ++index) {
        // A small tolerance avoids depending on the exact Windows timer tick.
        EXPECT_GE(progressTimes[index] - progressTimes[index - 1], 45ms);
    }
}

TEST(SchedulerTest, PendingObserverCanRequestShutdownReentrantly)
{
    Scheduler scheduler(testConfig(1));
    std::atomic<int> pendingNotifications{0};
    std::atomic<int> cancelledNotifications{0};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Pending
            && pendingNotifications.fetch_add(1) == 0) {
            scheduler.shutdown();
        } else if (snapshot.status == TaskStatus::Cancelled) {
            ++cancelledNotifications;
            scheduler.shutdown();
        }
    };

    const auto submitted = scheduler.submit(TaskSpec{
        "reentrant-shutdown",
        TaskLane::Compute,
        [](const TaskContext&) { return core::Result<bool>::success(true); },
        observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 100ms));
    ASSERT_TRUE(scheduler.snapshot(submitted.value()).has_value());
    EXPECT_EQ(
        scheduler.snapshot(submitted.value())->status,
        TaskStatus::Cancelled);
    EXPECT_EQ(pendingNotifications.load(), 1);
    EXPECT_EQ(cancelledNotifications.load(), 1);
}

TEST(SchedulerTest, PendingObserverCanDelegateShutdownAndWait)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> requestShutdownPromise;
    auto requestShutdown = requestShutdownPromise.get_future().share();
    std::promise<void> shutdownReturnedPromise;
    auto shutdownReturned = shutdownReturnedPromise.get_future().share();
    std::atomic<bool> delegated{false};
    std::atomic<bool> observerSawShutdownReturn{false};

    std::thread shutdownThread([&]() {
        requestShutdown.wait();
        scheduler.shutdown();
        shutdownReturnedPromise.set_value();
    });

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Pending
            && !delegated.exchange(true)) {
            requestShutdownPromise.set_value();
            observerSawShutdownReturn.store(
                shutdownReturned.wait_for(500ms)
                    == std::future_status::ready);
        }
    };

    const auto submitted = scheduler.submit(TaskSpec{
        "delegated-pending-shutdown",
        TaskLane::Compute,
        [](const TaskContext&) { return core::Result<bool>::success(true); },
        observer});

    ASSERT_EQ(shutdownReturned.wait_for(2s), std::future_status::ready);
    shutdownThread.join();
    ASSERT_TRUE(submitted) << submitted.error().message;
    EXPECT_TRUE(observerSawShutdownReturn.load());
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 100ms));
    EXPECT_EQ(
        scheduler.snapshot(submitted.value())->status,
        TaskStatus::Cancelled);
    scheduler.shutdownAndWait();
}

TEST(SchedulerTest, WorkerObserverCanRequestShutdownWithoutJoiningItself)
{
    Scheduler scheduler(testConfig(1));
    std::atomic<bool> requestedShutdown{false};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Running
            && !requestedShutdown.exchange(true)) {
            scheduler.shutdown();
        }
    };
    TaskAction action = [](const TaskContext& context) {
        if (context.stopRequested()) {
            return cancelledResult();
        }
        return core::Result<bool>::success(true);
    };

    const auto submitted = scheduler.submit(TaskSpec{
        "worker-shutdown", TaskLane::Compute, action, observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));
    ASSERT_TRUE(scheduler.snapshot(submitted.value()).has_value());
    EXPECT_EQ(
        scheduler.snapshot(submitted.value())->status,
        TaskStatus::Cancelled);
    EXPECT_TRUE(requestedShutdown.load());
}

TEST(SchedulerTest, RunningObserverCanDelegateShutdownAndWait)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> requestShutdownPromise;
    auto requestShutdown = requestShutdownPromise.get_future().share();
    std::promise<void> shutdownReturnedPromise;
    auto shutdownReturned = shutdownReturnedPromise.get_future().share();
    std::atomic<bool> delegated{false};
    std::atomic<bool> observerSawShutdownReturn{false};

    std::thread shutdownThread([&]() {
        requestShutdown.wait();
        scheduler.shutdown();
        shutdownReturnedPromise.set_value();
    });

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Running
            && !delegated.exchange(true)) {
            requestShutdownPromise.set_value();
            observerSawShutdownReturn.store(
                shutdownReturned.wait_for(500ms)
                    == std::future_status::ready);
        }
    };
    TaskAction action = [](const TaskContext& context) {
        if (context.stopRequested()) {
            return cancelledResult();
        }
        return core::Result<bool>::success(true);
    };

    const auto submitted = scheduler.submit(TaskSpec{
        "delegated-running-shutdown",
        TaskLane::Compute,
        action,
        observer});

    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_EQ(shutdownReturned.wait_for(2s), std::future_status::ready);
    shutdownThread.join();
    EXPECT_TRUE(observerSawShutdownReturn.load());
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));
    EXPECT_EQ(
        scheduler.snapshot(submitted.value())->status,
        TaskStatus::Cancelled);
    scheduler.shutdown();
}

TEST(SchedulerTest, ConcurrentWorkerObserversCanRequestShutdown)
{
    Scheduler scheduler(testConfig(2));
    std::atomic<int> shutdownRequests{0};
    std::promise<void> bothRequestedPromise;
    auto bothRequested = bothRequestedPromise.get_future();
    std::mutex runningMutex;
    std::condition_variable runningChanged;
    int runningObservers = 0;

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status != TaskStatus::Running) {
            return;
        }
        {
            std::unique_lock lock(runningMutex);
            ++runningObservers;
            runningChanged.notify_all();
            runningChanged.wait(lock, [&]() {
                return runningObservers == 2;
            });
        }
        if (shutdownRequests.fetch_add(1) + 1 == 2) {
            bothRequestedPromise.set_value();
        }
        scheduler.shutdown();
    };
    TaskAction action = [](const TaskContext& context) {
        if (context.stopRequested()) {
            return cancelledResult();
        }
        return core::Result<bool>::success(true);
    };

    const auto first = scheduler.submit(TaskSpec{
        "worker-shutdown-1", TaskLane::Compute, action, observer});
    const auto second = scheduler.submit(TaskSpec{
        "worker-shutdown-2", TaskLane::Compute, action, observer});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(bothRequested.wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(scheduler.waitForIdle(1s));
    EXPECT_EQ(shutdownRequests.load(), 2);
}

TEST(SchedulerTest, ConcurrentPendingObserversCanRequestShutdown)
{
    Scheduler scheduler(testConfig(2));
    std::barrier pendingObserversReady(2);
    std::atomic<int> shutdownRequests{0};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status != TaskStatus::Pending) {
            return;
        }
        pendingObserversReady.arrive_and_wait();
        ++shutdownRequests;
        scheduler.shutdown();
    };
    TaskAction action = [](const TaskContext&) {
        return core::Result<bool>::success(true);
    };

    auto submitTask = [&](std::string name) {
        return std::async(
            std::launch::async,
            [&, name = std::move(name)]() mutable {
                return scheduler.submit(TaskSpec{
                    std::move(name), TaskLane::Compute, action, observer});
            });
    };

    auto firstFuture = submitTask("pending-shutdown-1");
    auto secondFuture = submitTask("pending-shutdown-2");
    ASSERT_EQ(firstFuture.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(secondFuture.wait_for(1s), std::future_status::ready);

    const auto first = firstFuture.get();
    const auto second = secondFuture.get();
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    scheduler.shutdown();
    EXPECT_EQ(shutdownRequests.load(), 2);
    ASSERT_TRUE(scheduler.waitForIdle(100ms));
    EXPECT_EQ(scheduler.snapshot(first.value())->status, TaskStatus::Cancelled);
    EXPECT_EQ(scheduler.snapshot(second.value())->status, TaskStatus::Cancelled);
}

TEST(SchedulerTest, RepeatedHundredPercentReportsAreStillThrottled)
{
    SchedulerConfig config = testConfig(1);
    config.progressInterval = 50ms;
    Scheduler scheduler(config);
    std::atomic<int> hundredPercentNotifications{0};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Running
            && snapshot.progressPercent == 100) {
            ++hundredPercentNotifications;
        }
    };
    TaskAction action = [](const TaskContext& context) {
        for (int index = 0; index < 20; ++index) {
            context.reportProgress(100, "complete");
        }
        return core::Result<bool>::success(true);
    };

    const auto submitted = scheduler.submit(TaskSpec{
        "repeated-completion", TaskLane::Compute, action, observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));
    EXPECT_EQ(hundredPercentNotifications.load(), 1);
}

}  // namespace
}  // namespace signallab::task
