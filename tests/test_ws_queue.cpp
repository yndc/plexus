#include "../src/work_stealing_queue.h"
#include <atomic>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using namespace Plexus;

// Simple test type for queue testing
struct TestValue {
    int value;
    explicit TestValue(int v = 0) : value(v) {}
};

TEST(WorkStealingQueueTest, BasicPushPop) {
    WorkStealingQueue<TestValue> q(16);

    auto *v1 = new TestValue(1);
    auto *v2 = new TestValue(2);

    EXPECT_TRUE(q.push(v1));
    EXPECT_TRUE(q.push(v2));
    EXPECT_EQ(q.size_approx(), 2u);

    auto *p = q.pop();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->value, 2); // LIFO
    delete p;

    p = q.pop();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->value, 1);
    delete p;

    EXPECT_TRUE(q.empty_approx());
}

TEST(WorkStealingQueueTest, StealFIFO) {
    WorkStealingQueue<TestValue> q(16);

    q.push(new TestValue(10));
    q.push(new TestValue(20));

    auto *v = q.steal(); // FIFO
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->value, 10);
    delete v;

    auto *v2 = q.pop(); // LIFO
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v2->value, 20);
    delete v2;

    EXPECT_TRUE(q.empty_approx());
}

TEST(WorkStealingQueueTest, EmptyPopReturnsNullptr) {
    WorkStealingQueue<TestValue> q(16);
    EXPECT_EQ(q.pop(), nullptr);
}

TEST(WorkStealingQueueTest, EmptyStealReturnsNullptr) {
    WorkStealingQueue<TestValue> q(16);
    EXPECT_EQ(q.steal(), nullptr);
}

TEST(WorkStealingQueueTest, CapacityPowerOfTwo) {
    WorkStealingQueue<TestValue> q(10); // Not a power of 2
    EXPECT_EQ(q.capacity(), 16u);       // Rounded up to 16

    WorkStealingQueue<TestValue> q2(1);
    EXPECT_EQ(q2.capacity(), 2u); // Minimum is 2
}

TEST(WorkStealingQueueTest, QueueFullReturnsFalse) {
    WorkStealingQueue<TestValue> q(4);

    auto *v1 = new TestValue(1);
    auto *v2 = new TestValue(2);
    auto *v3 = new TestValue(3);
    auto *v4 = new TestValue(4);
    auto *v5 = new TestValue(5);

    EXPECT_TRUE(q.push(v1));
    EXPECT_TRUE(q.push(v2));
    EXPECT_TRUE(q.push(v3));
    EXPECT_TRUE(q.push(v4));
    EXPECT_FALSE(q.push(v5)); // Full

    delete v5; // Not pushed, must cleanup

    // Cleanup pushed items
    while (auto *p = q.pop()) {
        delete p;
    }
}

TEST(WorkStealingQueueTest, Contention) {
    WorkStealingQueue<TestValue> q(1024);
    std::atomic<bool> done{false};
    std::atomic<int> stolen_count{0};

    // Thief thread
    std::thread thief([&]() {
        while (!done) {
            auto *v = q.steal();
            if (v) {
                delete v;
                stolen_count++;
            }
        }
        // Drain rest
        while (true) {
            auto *v = q.steal();
            if (!v)
                break;
            delete v;
            stolen_count++;
        }
    });

    int pushed_count = 10000;
    int local_popped = 0;

    for (int i = 0; i < pushed_count; ++i) {
        auto *node = new TestValue(i);
        while (!q.push(node)) {
            // Full, pop some
            auto *v = q.pop();
            if (v) {
                delete v;
                local_popped++;
            }
        }

        // Randomly pop locally too
        if (i % 3 == 0) {
            auto *v = q.pop();
            if (v) {
                delete v;
                local_popped++;
            }
        }
    }

    done = true;
    thief.join();

    // Drain local
    while (true) {
        auto *v = q.pop();
        if (!v)
            break;
        delete v;
        local_popped++;
    }

    EXPECT_EQ(local_popped + stolen_count, pushed_count);
}

TEST(WorkStealingQueueTest, MultipleThieves) {
    WorkStealingQueue<TestValue> q(4096);
    std::atomic<bool> done{false};
    std::atomic<int> total_stolen{0};
    constexpr int NUM_THIEVES = 4;

    std::vector<std::thread> thieves;
    for (int t = 0; t < NUM_THIEVES; ++t) {
        thieves.emplace_back([&]() {
            int local_stolen = 0;
            while (!done) {
                auto *v = q.steal();
                if (v) {
                    delete v;
                    local_stolen++;
                }
            }
            while (true) {
                auto *v = q.steal();
                if (!v)
                    break;
                delete v;
                local_stolen++;
            }
            total_stolen.fetch_add(local_stolen, std::memory_order_relaxed);
        });
    }

    constexpr int PUSH_COUNT = 10000;
    int local_popped = 0;

    for (int i = 0; i < PUSH_COUNT; ++i) {
        auto *node = new TestValue(i);
        while (!q.push(node)) {
            auto *v = q.pop();
            if (v) {
                delete v;
                local_popped++;
            }
        }
    }

    done = true;
    for (auto &t : thieves) {
        t.join();
    }

    while (true) {
        auto *v = q.pop();
        if (!v)
            break;
        delete v;
        local_popped++;
    }

    EXPECT_EQ(local_popped + total_stolen.load(), PUSH_COUNT);
}
