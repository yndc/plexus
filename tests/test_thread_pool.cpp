#include "../src/thread_pool.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace Plexus;

// ===========================================================================
// FixedFunction Tests
// ===========================================================================

TEST(FixedFunctionTest, DefaultConstruction) {
    FixedFunction<64> ff;
    EXPECT_FALSE(static_cast<bool>(ff));
}

TEST(FixedFunctionTest, LambdaConstruction) {
    int value = 0;
    FixedFunction<64> ff([&value]() { value = 42; });

    EXPECT_TRUE(static_cast<bool>(ff));
    ff();
    EXPECT_EQ(value, 42);
}

TEST(FixedFunctionTest, MoveConstruction) {
    int value = 0;
    FixedFunction<64> ff1([&value]() { value = 100; });

    FixedFunction<64> ff2(std::move(ff1));
    EXPECT_TRUE(static_cast<bool>(ff2));
    // ff1 should be empty after move
    EXPECT_FALSE(static_cast<bool>(ff1));

    ff2();
    EXPECT_EQ(value, 100);
}

TEST(FixedFunctionTest, MoveAssignment) {
    int value = 0;
    FixedFunction<64> ff1([&value]() { value = 200; });
    FixedFunction<64> ff2;

    ff2 = std::move(ff1);
    EXPECT_TRUE(static_cast<bool>(ff2));
    EXPECT_FALSE(static_cast<bool>(ff1));

    ff2();
    EXPECT_EQ(value, 200);
}

TEST(FixedFunctionTest, SelfMoveAssignment) {
    int value = 0;
    FixedFunction<64> ff([&value]() { value = 300; });

// Suppress self-move warning for this specific test
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    ff = std::move(ff);
#pragma GCC diagnostic pop

    // Self-move should leave ff in a valid state
    EXPECT_TRUE(static_cast<bool>(ff));
    ff();
    EXPECT_EQ(value, 300);
}

TEST(FixedFunctionTest, CaptureByValue) {
    int captured = 42;
    FixedFunction<64> ff([captured]() mutable {
        // Lambda captures by value
        captured += 1;
    });

    ff();
    EXPECT_EQ(captured, 42); // Original unchanged
}

// ===========================================================================
// ThreadPool Tests
// ===========================================================================

TEST(ThreadPoolTest, SingleTaskEnqueueAndWait) {
    ThreadPool pool;
    std::atomic<int> count{0};

    pool.enqueue([&count]() { count++; });
    pool.wait();

    EXPECT_EQ(count, 1);
}

TEST(ThreadPoolTest, BatchDispatch) {
    ThreadPool pool;
    std::atomic<int> count{0};
    constexpr int NUM_TASKS = 100;

    std::vector<ThreadPool::Task> tasks;
    for (int i = 0; i < NUM_TASKS; ++i) {
        tasks.emplace_back([&count]() { count++; });
    }

    pool.dispatch(std::move(tasks));
    pool.wait();

    EXPECT_EQ(count, NUM_TASKS);
}

TEST(ThreadPoolTest, EmptyDispatchNoOp) {
    ThreadPool pool;
    std::vector<ThreadPool::Task> tasks;
    pool.dispatch(std::move(tasks)); // Should be no-op
    pool.wait();                     // Should return immediately
}

TEST(ThreadPoolTest, ExceptionInTask) {
    ThreadPool pool;
    std::atomic<bool> second_ran{false};

    pool.enqueue([]() { throw std::runtime_error("test exception"); });
    pool.enqueue([&second_ran]() { second_ran = true; });
    pool.wait();

    // Exception in one task shouldn't prevent others
    EXPECT_TRUE(second_ran);
}

TEST(ThreadPoolTest, HighContention) {
    ThreadPool pool;
    std::atomic<int> count{0};
    constexpr int NUM_TASKS = 10000;

    for (int i = 0; i < NUM_TASKS; ++i) {
        pool.enqueue([&count]() { count++; });
    }
    pool.wait();

    EXPECT_EQ(count, NUM_TASKS);
}

TEST(ThreadPoolTest, WorkerThreadIdentity) {
    ThreadPool pool(2);
    std::atomic<int> worker_count{0};
    std::atomic<int> non_worker_count{0};

    // Enqueue from main thread (non-worker)
    for (int i = 0; i < 10; ++i) {
        pool.enqueue([&worker_count, &non_worker_count]() {
            if (t_worker_index != SIZE_MAX) {
                worker_count++;
            } else {
                non_worker_count++;
            }
        });
    }
    pool.wait();

    // All tasks should have run on worker threads
    EXPECT_EQ(worker_count, 10);
    EXPECT_EQ(non_worker_count, 0);
}

TEST(ThreadPoolTest, WaitMultipleTimes) {
    ThreadPool pool;
    std::atomic<int> count{0};

    pool.enqueue([&count]() { count++; });
    pool.wait();
    EXPECT_EQ(count, 1);

    pool.enqueue([&count]() { count++; });
    pool.wait();
    EXPECT_EQ(count, 2);
}

TEST(ThreadPoolTest, ZeroInitialThreadsFallback) {
    // When hardware_concurrency is 0, fall back to 2
    // We can't easily test this, but we can test with explicit count
    ThreadPool pool(1);
    std::atomic<int> count{0};

    pool.enqueue([&count]() { count++; });
    pool.wait();

    EXPECT_EQ(count, 1);
}

TEST(ThreadPoolTest, TasksFromWorkerThread) {
    ThreadPool pool;
    std::atomic<int> count{0};

    // Task that enqueues more tasks from within a worker
    pool.enqueue([&pool, &count]() {
        count++;
        pool.enqueue([&count]() { count++; });
        pool.enqueue([&count]() { count++; });
    });
    pool.wait();

    EXPECT_EQ(count, 3);
}
