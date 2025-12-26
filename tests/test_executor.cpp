#include "plexus/executor.h"
#include <atomic>
#include <gtest/gtest.h>

TEST(ExecutorTest, BasicExecution) {
    Plexus::ThreadPool pool;
    Plexus::Executor executor(pool);

    std::atomic<int> count{0};

    // Setup Graph (Simple task counting executions)
    Plexus::ExecutionGraph graph;
    graph.nodes.push_back({[&]() { count++; }, {}, 0});
    graph.entry_nodes.push_back(0);

    executor.run(graph);
    EXPECT_EQ(count, 1);
}

TEST(ExecutorTest, Profiling) {
    Plexus::ThreadPool pool;
    Plexus::Executor executor(pool);

    bool callback_invoked = false;
    executor.set_profiler_callback([&](const char *name, double duration) {
        callback_invoked = true;
        EXPECT_GE(duration, 0.0);
    });

    Plexus::ExecutionGraph graph;
    graph.nodes.push_back({[]() {}, {}, 0});
    graph.entry_nodes.push_back(0);

    executor.run(graph);
    EXPECT_TRUE(callback_invoked);
}

TEST(ExecutorTest, MultiNodeGraph) {
    Plexus::ThreadPool pool;
    Plexus::Executor executor(pool);

    std::atomic<int> counter{0};

    // Graph: A -> B
    Plexus::ExecutionGraph graph;

    // Node 0 (A)
    graph.nodes.push_back({
        [&]() {
            int expected = 0;
            counter.compare_exchange_strong(expected, 1);
        },
        {1}, // Dependents: Node 1
        0    // Initial deps: 0
    });

    // Node 1 (B)
    graph.nodes.push_back({
        [&]() {
            int expected = 1;
            counter.compare_exchange_strong(expected, 2);
        },
        {}, // Dependents: None
        1   // Initial deps: 1 (Node 0)
    });

    graph.entry_nodes.push_back(0);

    executor.run(graph);

    EXPECT_EQ(counter, 2);
}
