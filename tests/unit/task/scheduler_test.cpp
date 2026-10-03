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
    std::promise<void> observerFinishedPromise;
    auto observerFinished = observerFinishedPromise.get_future();
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
            observerFinishedPromise.set_value();
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
    ASSERT_EQ(observerFinished.wait_for(2s), std::future_status::ready);
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

TEST(SchedulerTest, RemoveRejectsUnknownPendingAndRunningTasks)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> startedPromise;
    auto started = startedPromise.get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();

    const auto running = scheduler.submit(TaskSpec{
        "running-remove",
        TaskLane::Compute,
        [&](const TaskContext&) {
            startedPromise.set_value();
            release.wait();
            return core::Result<bool>::success(true);
        },
        {}});
    ASSERT_TRUE(running) << running.error().message;
    if (started.wait_for(1s) != std::future_status::ready) {
        releasePromise.set_value();
        FAIL() << "The running task did not start before the timeout.";
    }

    const auto pending = scheduler.submit(TaskSpec{
        "pending-remove",
        TaskLane::Compute,
        [](const TaskContext&) {
            return core::Result<bool>::success(true);
        },
        {}});
    if (!pending) {
        releasePromise.set_value();
        FAIL() << pending.error().message;
    }

    EXPECT_FALSE(scheduler.remove(999999));
    EXPECT_FALSE(scheduler.remove(running.value()));
    EXPECT_FALSE(scheduler.remove(pending.value()));
    EXPECT_TRUE(scheduler.snapshot(running.value()).has_value());
    EXPECT_TRUE(scheduler.snapshot(pending.value()).has_value());

    releasePromise.set_value();
    ASSERT_TRUE(scheduler.waitForIdle(1s));
}

TEST(SchedulerTest, RemoveErasesTerminalTaskAndReleasesCallbacks)
{
    Scheduler scheduler(testConfig(1));
    auto actionLifetime = std::make_shared<int>(1);
    auto observerLifetime = std::make_shared<int>(2);
    const std::weak_ptr<int> weakAction = actionLifetime;
    const std::weak_ptr<int> weakObserver = observerLifetime;
    std::promise<void> terminalObservedPromise;
    auto terminalObserved = terminalObservedPromise.get_future();

    TaskAction action = [lifetime = actionLifetime](const TaskContext&) {
        static_cast<void>(lifetime);
        return core::Result<bool>::success(true);
    };
    TaskObserver observer = [
        lifetime = observerLifetime,
        &terminalObservedPromise](const TaskSnapshot& snapshot) {
        static_cast<void>(lifetime);
        if (isTerminal(snapshot.status)) {
            terminalObservedPromise.set_value();
        }
    };
    const auto submitted = scheduler.submit(TaskSpec{
        "remove-terminal",
        TaskLane::Compute,
        std::move(action),
        std::move(observer)});
    ASSERT_TRUE(submitted) << submitted.error().message;
    actionLifetime.reset();
    observerLifetime.reset();

    ASSERT_TRUE(scheduler.waitForTerminal(submitted.value(), 1s));
    ASSERT_EQ(terminalObserved.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(weakAction.expired());
    EXPECT_FALSE(weakObserver.expired());

    EXPECT_TRUE(scheduler.remove(submitted.value()));
    EXPECT_FALSE(scheduler.snapshot(submitted.value()).has_value());
    EXPECT_TRUE(scheduler.snapshots().empty());
    EXPECT_FALSE(scheduler.retry(submitted.value()));
    EXPECT_TRUE(weakAction.expired());

    // The terminal dispatcher may still own the removed record briefly while
    // returning from its callback.
    for (int attempt = 0; attempt < 100 && !weakObserver.expired(); ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(weakObserver.expired());
}

TEST(SchedulerTest, TerminalObserverCanRemoveItsOwnTask)
{
    Scheduler scheduler(testConfig(1));
    std::promise<bool> removedPromise;
    auto removed = removedPromise.get_future();

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (isTerminal(snapshot.status)) {
            removedPromise.set_value(scheduler.remove(snapshot.id));
        }
    };
    const auto submitted = scheduler.submit(TaskSpec{
        "self-remove",
        TaskLane::Compute,
        [](const TaskContext&) {
            return core::Result<bool>::success(true);
        },
        observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    ASSERT_EQ(removed.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(removed.get());
    EXPECT_FALSE(scheduler.snapshot(submitted.value()).has_value());
    EXPECT_TRUE(scheduler.snapshots().empty());
}

TEST(SchedulerTest, ExistingTerminalWaiterSurvivesObserverRemoval)
{
    Scheduler scheduler(testConfig(1));
    std::promise<void> actionStartedPromise;
    auto actionStarted = actionStartedPromise.get_future();
    std::promise<void> releaseActionPromise;
    auto releaseAction = releaseActionPromise.get_future().share();
    std::promise<void> waiterStartingPromise;
    auto waiterStarting = waiterStartingPromise.get_future();
    std::promise<bool> removedPromise;
    auto removed = removedPromise.get_future();

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (isTerminal(snapshot.status)) {
            removedPromise.set_value(scheduler.remove(snapshot.id));
        }
    };
    const auto submitted = scheduler.submit(TaskSpec{
        "wait-while-removing",
        TaskLane::Compute,
        [&](const TaskContext&) {
            actionStartedPromise.set_value();
            releaseAction.wait();
            return core::Result<bool>::success(true);
        },
        observer});
    ASSERT_TRUE(submitted) << submitted.error().message;
    if (actionStarted.wait_for(1s) != std::future_status::ready) {
        releaseActionPromise.set_value();
        scheduler.shutdownAndWait();
        FAIL() << "The gated task did not start before the timeout.";
    }

    auto waiter = std::async(std::launch::async, [&]() {
        waiterStartingPromise.set_value();
        return scheduler.waitForTerminal(submitted.value(), 1s);
    });
    if (waiterStarting.wait_for(1s) != std::future_status::ready) {
        releaseActionPromise.set_value();
        scheduler.shutdownAndWait();
        static_cast<void>(waiter.wait());
        FAIL() << "The terminal waiter did not start before the timeout.";
    }
    // Give waitForTerminal() time to acquire and release the scheduler lock;
    // the action remains gated, so the task cannot finish during this setup.
    std::this_thread::sleep_for(20ms);

    releaseActionPromise.set_value();
    ASSERT_EQ(removed.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(removed.get());
    ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(waiter.get());
    EXPECT_FALSE(scheduler.snapshot(submitted.value()).has_value());
}

TEST(SchedulerTest, RemovePreservesQueuedTerminalNotification)
{
    Scheduler scheduler(testConfig(1));
    std::promise<TaskId> pendingObservedPromise;
    auto pendingObserved = pendingObservedPromise.get_future();
    std::promise<void> releasePendingPromise;
    auto releasePending = releasePendingPromise.get_future().share();
    std::atomic<int> terminalNotifications{0};

    TaskObserver observer = [&](const TaskSnapshot& snapshot) {
        if (snapshot.status == TaskStatus::Pending) {
            pendingObservedPromise.set_value(snapshot.id);
            releasePending.wait();
        } else if (isTerminal(snapshot.status)) {
            ++terminalNotifications;
        }
    };

    auto submission = std::async(std::launch::async, [&]() {
        return scheduler.submit(TaskSpec{
            "remove-queued-notification",
            TaskLane::Compute,
            [](const TaskContext&) {
                return core::Result<bool>::success(true);
            },
            observer});
    });

    if (pendingObserved.wait_for(1s) != std::future_status::ready) {
        releasePendingPromise.set_value();
        scheduler.shutdownAndWait();
        static_cast<void>(submission.wait());
        FAIL() << "The pending observer did not run before the timeout.";
    }
    const TaskId id = pendingObserved.get();
    scheduler.shutdown();
    const auto cancelledSnapshot = scheduler.snapshot(id);
    if (!cancelledSnapshot.has_value()) {
        releasePendingPromise.set_value();
        scheduler.shutdownAndWait();
        static_cast<void>(submission.wait());
        FAIL() << "The cancelled task snapshot disappeared unexpectedly.";
    }
    EXPECT_EQ(cancelledSnapshot->status, TaskStatus::Cancelled);
    EXPECT_TRUE(scheduler.remove(id));

    releasePendingPromise.set_value();
    ASSERT_EQ(submission.wait_for(1s), std::future_status::ready);
    const auto submitted = submission.get();
    ASSERT_TRUE(submitted) << submitted.error().message;
    EXPECT_EQ(submitted.value(), id);
    EXPECT_EQ(terminalNotifications.load(), 1);
    EXPECT_FALSE(scheduler.snapshot(id).has_value());
    scheduler.shutdownAndWait();
}

TEST(SchedulerTest, ConcurrentRetryAndRemoveHaveALinearizedOutcome)
{
    Scheduler scheduler(testConfig(1));
    const auto original = scheduler.submit(TaskSpec{
        "retry-remove-race",
        TaskLane::Compute,
        [](const TaskContext&) {
            return core::Result<bool>::success(true);
        },
        {}});
    ASSERT_TRUE(original) << original.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(original.value(), 1s));

    std::barrier start(3);
    auto retryFuture = std::async(std::launch::async, [&]() {
        start.arrive_and_wait();
        return scheduler.retry(original.value());
    });
    auto removeFuture = std::async(std::launch::async, [&]() {
        start.arrive_and_wait();
        return scheduler.remove(original.value());
    });
    start.arrive_and_wait();

    ASSERT_EQ(retryFuture.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(removeFuture.wait_for(1s), std::future_status::ready);
    const auto retried = retryFuture.get();
    EXPECT_TRUE(removeFuture.get());
    EXPECT_FALSE(scheduler.snapshot(original.value()).has_value());

    if (retried) {
        ASSERT_TRUE(scheduler.waitForTerminal(retried.value(), 1s));
        const auto retrySnapshot = scheduler.snapshot(retried.value());
        ASSERT_TRUE(retrySnapshot.has_value());
        EXPECT_EQ(retrySnapshot->retryOf, original.value());
    } else {
        EXPECT_EQ(retried.error().code, core::ErrorCode::InvalidParameter);
    }
}

}  // namespace
}  // namespace signallab::task
