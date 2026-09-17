#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

#include "signallab/task/thread_pool.hpp"

namespace signallab::task {
namespace {

using namespace std::chrono_literals;

TEST(ThreadPoolTest, NeverRunsMoreThanItsFixedWorkerCount)
{
    ThreadPool pool(3);
    std::mutex mutex;
    std::condition_variable changed;
    int active = 0;
    int maximumActive = 0;
    int started = 0;
    int completed = 0;
    bool release = false;
    std::promise<void> allCompleted;
    auto completion = allCompleted.get_future();

    for (TaskId id = 1; id <= 9; ++id) {
        ASSERT_TRUE(pool.submit(QueuedTask{
            id,
            [&]() {
                std::unique_lock lock(mutex);
                ++active;
                ++started;
                maximumActive = std::max(maximumActive, active);
                changed.notify_all();
                changed.wait(lock, [&release]() { return release; });
                --active;
                ++completed;
                if (completed == 9) {
                    allCompleted.set_value();
                }
            }}));
    }

    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 1s, [&started]() {
            return started == 3;
        }));
        EXPECT_EQ(active, 3);
        EXPECT_EQ(maximumActive, 3);
        release = true;
    }
    changed.notify_all();

    ASSERT_EQ(completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completed, 9);
    EXPECT_EQ(maximumActive, 3);
    EXPECT_EQ(pool.workerCount(), 3U);
    pool.shutdown();
}

TEST(ThreadPoolTest, ContainsAnExceptionAndContinuesWithTheNextItem)
{
    ThreadPool pool(1);
    std::promise<void> secondRan;
    auto completion = secondRan.get_future();

    ASSERT_TRUE(pool.submit(QueuedTask{
        1,
        []() { throw 42; }}));
    ASSERT_TRUE(pool.submit(QueuedTask{
        2,
        [&secondRan]() { secondRan.set_value(); }}));

    ASSERT_EQ(completion.wait_for(1s), std::future_status::ready);
    pool.shutdown();
}

}  // namespace
}  // namespace signallab::task
