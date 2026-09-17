#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <vector>

#include "signallab/task/task_queue.hpp"

namespace signallab::task {
namespace {

using namespace std::chrono_literals;

TEST(TaskQueueTest, PreservesFifoOrderAndRejectsPushAfterClose)
{
    TaskQueue queue;
    ASSERT_TRUE(queue.push(QueuedTask{1, []() {}}));
    ASSERT_TRUE(queue.push(QueuedTask{2, []() {}}));
    ASSERT_TRUE(queue.push(QueuedTask{3, []() {}}));

    const auto first = queue.waitPop();
    const auto second = queue.waitPop();
    const auto third = queue.waitPop();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(first->id, 1U);
    EXPECT_EQ(second->id, 2U);
    EXPECT_EQ(third->id, 3U);

    queue.close();
    EXPECT_FALSE(queue.push(QueuedTask{4, []() {}}));
    EXPECT_FALSE(queue.waitPop().has_value());
}

TEST(TaskQueueTest, CloseWakesAWaitingConsumer)
{
    TaskQueue queue;
    std::promise<bool> completed;
    auto completion = completed.get_future();

    std::jthread consumer([&queue, &completed](std::stop_token token) {
        completed.set_value(!queue.waitPop(token).has_value());
    });

    queue.close();

    ASSERT_EQ(completion.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(completion.get());
}

TEST(TaskQueueTest, CanRemoveOnePendingTaskById)
{
    TaskQueue queue;
    ASSERT_TRUE(queue.push(QueuedTask{10, []() {}}));
    ASSERT_TRUE(queue.push(QueuedTask{20, []() {}}));

    const auto removed = queue.remove(10);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->id, 10U);

    const auto remaining = queue.waitPop();
    ASSERT_TRUE(remaining.has_value());
    EXPECT_EQ(remaining->id, 20U);
    EXPECT_FALSE(queue.remove(999).has_value());
}

}  // namespace
}  // namespace signallab::task
