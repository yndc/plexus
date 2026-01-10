#include "../src/work_stealing_queue.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace Plexus;

TEST(WorkStealingQueueTest, BasicPushPop) {
    WorkStealingQueue<int> q(16);
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_EQ(q.size_approx(), 2u);

    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 2); // LIFO

    v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1);

    EXPECT_TRUE(q.empty_approx());
}

TEST(WorkStealingQueueTest, StealFIFO) {
    WorkStealingQueue<int> q(16);
    q.push(10);
    q.push(20);

    auto v = q.steal(); // FIFO
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 10);

    auto v2 = q.pop(); // LIFO
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 20);

    EXPECT_TRUE(q.empty_approx());
}

TEST(WorkStealingQueueTest, EmptyPopReturnsNullopt) {
    WorkStealingQueue<int> q(16);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(WorkStealingQueueTest, EmptyStealReturnsNullopt) {
    WorkStealingQueue<int> q(16);
    EXPECT_FALSE(q.steal().has_value());
}

TEST(WorkStealingQueueTest, CapacityPowerOfTwo) {
    WorkStealingQueue<int> q(10); // Not a power of 2
    EXPECT_EQ(q.capacity(), 16u); // Rounded up to 16

    WorkStealingQueue<int> q2(1);
    EXPECT_EQ(q2.capacity(), 2u); // Minimum is 2
}

TEST(WorkStealingQueueTest, QueueFullReturnsFalse) {
    WorkStealingQueue<int> q(4);
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5)); // Full
}

TEST(WorkStealingQueueTest, Contention) {
    WorkStealingQueue<int> q(1024);
    std::atomic<bool> done{false};
    std::atomic<int> stolen_count{0};

    // Thief thread
    std::thread thief([&]() {
        while (!done) {
            auto v = q.steal();
            if (v.has_value()) {
                stolen_count++;
            }
        }
        // Drain rest
        while (true) {
            auto v = q.steal();
            if (!v.has_value())
                break;
            stolen_count++;
        }
    });

    int pushed_count = 10000;
    int local_popped = 0;

    for (int i = 0; i < pushed_count; ++i) {
        while (!q.push(i)) {
            // Full, pop some
            auto v = q.pop();
            if (v.has_value())
                local_popped++;
        }

        // Randomly pop locally too
        if (i % 3 == 0) {
            auto v = q.pop();
            if (v.has_value())
                local_popped++;
        }
    }

    done = true;
    thief.join();

    // Drain local
    while (true) {
        auto v = q.pop();
        if (!v.has_value())
            break;
        local_popped++;
    }

    EXPECT_EQ(local_popped + stolen_count, pushed_count);
}

TEST(WorkStealingQueueTest, MultipleThieves) {
    WorkStealingQueue<int> q(4096);
    std::atomic<bool> done{false};
    std::atomic<int> total_stolen{0};
    constexpr int NUM_THIEVES = 4;

    std::vector<std::thread> thieves;
    for (int t = 0; t < NUM_THIEVES; ++t) {
        thieves.emplace_back([&]() {
            int local_stolen = 0;
            while (!done) {
                auto v = q.steal();
                if (v.has_value()) {
                    local_stolen++;
                }
            }
            while (true) {
                auto v = q.steal();
                if (!v.has_value())
                    break;
                local_stolen++;
            }
            total_stolen.fetch_add(local_stolen, std::memory_order_relaxed);
        });
    }

    constexpr int PUSH_COUNT = 10000;
    int local_popped = 0;

    for (int i = 0; i < PUSH_COUNT; ++i) {
        while (!q.push(i)) {
            auto v = q.pop();
            if (v.has_value())
                local_popped++;
        }
    }

    done = true;
    for (auto &t : thieves) {
        t.join();
    }

    while (true) {
        auto v = q.pop();
        if (!v.has_value())
            break;
        local_popped++;
    }

    EXPECT_EQ(local_popped + total_stolen.load(), PUSH_COUNT);
}

TEST(WorkStealingQueueTest, MoveOnlyType) {
    WorkStealingQueue<std::unique_ptr<int>> q(16);
    q.push(std::make_unique<int>(42));

    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(**v, 42);
}
