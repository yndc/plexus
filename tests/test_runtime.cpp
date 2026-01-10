#include "plexus/context.h"
#include "plexus/executor.h"
#include "plexus/graph_builder.h"
#include "plexus/runtime.h"
#include <atomic>
#include <gtest/gtest.h>
#include <set>

using namespace Plexus;

// ===========================================================================
// Worker Index Tests
// ===========================================================================

TEST(RuntimeTest, WorkerIndexOutsideExecution) {
    // Outside any Plexus context, should return SIZE_MAX
    EXPECT_EQ(current_worker_index(), SIZE_MAX);
    EXPECT_EQ(worker_count(), 0u);
}

TEST(RuntimeTest, WorkerIndexFromWorkerThread) {
    Context ctx;
    Executor executor;
    std::atomic<size_t> max_seen{0};
    std::atomic<size_t> observed_worker_count{0};
    std::set<size_t> seen_indices;
    std::mutex mtx;

    GraphBuilder builder(ctx);
    constexpr int NUM_TASKS = 100;

    for (int i = 0; i < NUM_TASKS; ++i) {
        builder.add_node(
            {.debug_name = "task" + std::to_string(i), .work_function = [&]() {
                 size_t idx = current_worker_index();
                 size_t wc = worker_count();
                 observed_worker_count.store(wc, std::memory_order_relaxed);

                 EXPECT_NE(idx, SIZE_MAX) << "Worker index should not be SIZE_MAX inside execution";
                 EXPECT_LT(idx, wc) << "Worker index should be < worker_count()";

                 // Track max and unique indices
                 size_t prev_max = max_seen.load();
                 while (prev_max < idx && !max_seen.compare_exchange_weak(prev_max, idx)) {
                 }

                 {
                     std::lock_guard<std::mutex> lock(mtx);
                     seen_indices.insert(idx);
                 }
             }});
    }

    executor.run(builder.bake());

    // Should have seen multiple worker indices (unless single-core machine)
    EXPECT_GE(seen_indices.size(), 1u);
    EXPECT_LE(max_seen.load(), observed_worker_count.load());
}

TEST(RuntimeTest, WorkerCountConsistency) {
    Context ctx;
    Executor executor;
    std::atomic<size_t> observed_count{0};

    GraphBuilder builder(ctx);
    builder.add_node(
        {.debug_name = "check", .work_function = [&]() { observed_count = worker_count(); }});

    executor.run(builder.bake());

    // Worker count should be > 0 during execution
    EXPECT_GT(observed_count.load(), 0u);
}

TEST(RuntimeTest, WorkerIndexSequentialMode) {
    Context ctx;
    Executor executor;
    size_t observed_index = SIZE_MAX;
    size_t observed_count = 0;

    GraphBuilder builder(ctx);
    builder.add_node({.debug_name = "sequential_task", .work_function = [&]() {
                          observed_index = current_worker_index();
                          observed_count = worker_count();
                      }});

    executor.run(builder.bake(), ExecutionMode::Sequential);

    // Sequential mode: single logical worker at index 0
    EXPECT_EQ(observed_index, 0u);
    EXPECT_EQ(observed_count, 1u);
}

TEST(RuntimeTest, WorkerIndexMainThreadAffinity) {
    Context ctx;
    Executor executor;
    size_t main_thread_index = SIZE_MAX;
    size_t main_thread_count = 0;
    size_t worker_thread_index = SIZE_MAX;
    size_t worker_thread_count = 0;

    GraphBuilder builder(ctx);

    // Worker thread task runs first
    NodeID worker_node = builder.add_node({.debug_name = "worker_task", .work_function = [&]() {
                                               worker_thread_index = current_worker_index();
                                               worker_thread_count = worker_count();
                                           }});

    // Main thread task depends on worker task
    builder.add_node({.debug_name = "main_task",
                      .work_function =
                          [&]() {
                              main_thread_index = current_worker_index();
                              main_thread_count = worker_count();
                          },
                      .run_after = {worker_node},
                      .thread_affinity = ThreadAffinity::Main});

    executor.run(builder.bake());

    // Worker should have index < worker_count
    EXPECT_LT(worker_thread_index, worker_thread_count);

    // Main thread gets designated index = worker_count
    EXPECT_EQ(main_thread_index, main_thread_count);
    EXPECT_GT(main_thread_count, 0u);
}

TEST(RuntimeTest, WorkerIndexRestoredAfterExecution) {
    size_t before = current_worker_index();
    size_t count_before = worker_count();

    {
        Context ctx;
        Executor executor;
        GraphBuilder builder(ctx);
        builder.add_node({.debug_name = "task", .work_function = []() {}});
        executor.run(builder.bake());
    }

    // Values should be unchanged after execution completes
    EXPECT_EQ(current_worker_index(), before);
    EXPECT_EQ(worker_count(), count_before);
}
