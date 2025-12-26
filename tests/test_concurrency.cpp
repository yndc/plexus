#include "plexus/context.h"
#include "plexus/graph_builder.h"
#include "plexus/thread_pool.h"
#include <atomic>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>

// Helper to execute async graph in tests
void execute_graph_test(Plexus::ThreadPool &pool, const Plexus::ExecutionGraph &graph) {
    if (graph.nodes.empty())
        return;

    std::unique_ptr<std::atomic<int>[]> counters(new std::atomic<int>[graph.nodes.size()]);
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        counters[i].store(graph.nodes[i].initial_dependencies, std::memory_order_relaxed);
    }

    std::atomic<int> *counters_ptr = counters.get();

    // We use a fix-point style lambda for recursion
    // Since we can't easily capture local lambda by itself, we use a std::function
    // or just a strict Y-comb equivalent. For simplicity in test: std::function.
    // Note: capturing std::function by reference is dangerous if it goes out of scope,
    // but here we wait() at the end, so stack is valid.

    std::function<void(int)> run_recursive;
    run_recursive = [&](int node_idx) {
        if (graph.nodes[node_idx].work) {
            graph.nodes[node_idx].work();
        }

        for (int dep_idx : graph.nodes[node_idx].dependents) {
            int prev = counters_ptr[dep_idx].fetch_sub(1, std::memory_order_release);
            if (prev == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                pool.enqueue([=]() { run_recursive(dep_idx); });
            }
        }
    };

    for (int node_idx : graph.entry_nodes) {
        pool.enqueue([=]() { run_recursive(node_idx); });
    }

    pool.wait();
    pool.wait();
}

TEST(ConcurrencyTest, RunGraph) {
    Plexus::Context ctx;
    auto res_id = ctx.register_resource("SharedData");

    Plexus::GraphBuilder builder(ctx);

    std::atomic<int> counter{0};
    std::atomic<int> nodes_read_correct_value{0};
    const int num_readers = 100;

    // Node: Writer (Runs first)
    builder.add_node({"Writer", [&]() { counter = 1000; }, {{res_id, Plexus::Access::WRITE}}});

    // Nodes: Readers (Run parallel after Writer)
    for (int i = 0; i < num_readers; ++i) {
        builder.add_node({"Reader",
                          [&]() {
                              if (counter == 1000) {
                                  nodes_read_correct_value++;
                              }
                          },
                          {{res_id, Plexus::Access::READ}}});
    }

    // Node: Final Writer (Runs after all Readers)
    builder.add_node({"Finalizer", [&]() { counter = 2000; }, {{res_id, Plexus::Access::WRITE}}});

    auto graph = builder.bake();
    Plexus::ThreadPool pool;

    execute_graph_test(pool, graph);

    EXPECT_EQ(counter, 2000);
    EXPECT_EQ(nodes_read_correct_value, num_readers);
}

TEST(ConcurrencyTest, IndependentTasks) {
    Plexus::ThreadPool pool;
    std::atomic<int> count{0};

    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 50; ++i) {
        tasks.push_back([&]() { count++; });
    }

    pool.dispatch(tasks);
    pool.wait();

    EXPECT_EQ(count, 50);
}

TEST(ConcurrencyTest, StressRandomGraph) {
    Plexus::ThreadPool pool;
    Plexus::Context ctx;
    Plexus::GraphBuilder builder(ctx);

    const int NUM_NODES = 100;
    const int NUM_RESOURCES = 10;

    // Create resources
    std::vector<Plexus::ResourceID> resources;
    for (int i = 0; i < NUM_RESOURCES; ++i) {
        resources.push_back(ctx.register_resource("Res_" + std::to_string(i)));
    }

    // Shared data to detect races or order issues.
    struct WriteRecord {
        int node_id;
        int order_ticket;
    };
    std::vector<std::vector<WriteRecord>> resource_data(NUM_RESOURCES);

    std::mt19937 rng(42);
    std::vector<int> expected_writes(NUM_RESOURCES, 0);

    // Global atomic counter to track execution order
    std::atomic<int> global_ticket{0};

    for (int i = 0; i < NUM_NODES; ++i) {
        Plexus::NodeConfig config;
        config.debug_name = "Node_" + std::to_string(i);

        int num_deps = std::uniform_int_distribution<>(1, 3)(rng);
        for (int j = 0; j < num_deps; ++j) {
            int res_idx = std::uniform_int_distribution<>(0, NUM_RESOURCES - 1)(rng);
            auto access = (std::uniform_int_distribution<>(0, 1)(rng) == 0) ? Plexus::Access::READ
                                                                            : Plexus::Access::WRITE;

            bool exists = false;
            for (const auto &d : config.dependencies)
                if (d.id == resources[res_idx])
                    exists = true;
            if (!exists) {
                config.dependencies.push_back({resources[res_idx], access});
                if (access == Plexus::Access::WRITE) {
                    expected_writes[res_idx]++;
                }
            }
        }

        // Work Function
        config.work_function = [i, &resource_data, config, &global_ticket]() {
            // Grab ticket for this run
            int ticket = global_ticket.fetch_add(1);

            for (const auto &dep : config.dependencies) {
                if (dep.access == Plexus::Access::WRITE) {
                    resource_data[dep.id].push_back({i, ticket});
                } else {
                    // Read: check size
                    volatile size_t s = resource_data[dep.id].size();
                    (void)s;
                }
            }
        };

        builder.add_node(config);
    }

    auto graph = builder.bake();

    // Execute
    execute_graph_test(pool, graph);

    // VERIFICATION
    for (int i = 0; i < NUM_RESOURCES; ++i) {
        EXPECT_EQ(resource_data[i].size(), expected_writes[i]);

        // Check strict ordering for writes
        int last_ticket = -1;
        for (const auto &record : resource_data[i]) {
            EXPECT_GT(record.order_ticket, last_ticket) << "Writes out of order on resource " << i;
            last_ticket = record.order_ticket;
        }
    }
}

TEST(ConcurrencyTest, WriteAfterRead) {
    Plexus::Context ctx;
    auto res_id = ctx.register_resource("Shared");
    Plexus::GraphBuilder builder(ctx);

    std::atomic<int> read_count{0};
    bool write_happened = false;
    bool write_saw_all_reads = false;

    // 2 Readers
    builder.add_node({"Reader1", [&]() { read_count++; }, {{res_id, Plexus::Access::READ}}});
    builder.add_node({"Reader2", [&]() { read_count++; }, {{res_id, Plexus::Access::READ}}});

    // 1 Writer (must run AFTER readers)
    builder.add_node({"Writer",
                      [&]() {
                          write_happened = true;
                          if (read_count == 2) {
                              write_saw_all_reads = true;
                          }
                      },
                      {{res_id, Plexus::Access::WRITE}}});

    auto graph = builder.bake();
    Plexus::ThreadPool pool;

    execute_graph_test(pool, graph);

    EXPECT_TRUE(write_happened);
    EXPECT_TRUE(write_saw_all_reads);
    EXPECT_EQ(read_count, 2);
}
