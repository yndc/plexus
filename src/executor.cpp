#include "plexus/executor.h"
#include <memory>

namespace Plexus {

    Executor::Executor(ThreadPool &pool) : m_pool(pool) {}

    void Executor::run(const ExecutionGraph &graph) {
        if (graph.nodes.empty())
            return;

        auto start = std::chrono::high_resolution_clock::now();

        // 1. Initialize State
        // Use unique_ptr array for counters because std::atomic is not copyable/movable
        std::unique_ptr<std::atomic<int>[]> counters(new std::atomic<int>[graph.nodes.size()]);
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            counters[i].store(graph.nodes[i].initial_dependencies, std::memory_order_relaxed);
        }

        std::atomic<int> *counters_ptr = counters.get();

        // 2. Submit Entry Nodes
        for (int node_idx : graph.entry_nodes) {
            m_pool.enqueue([this, &graph, counters_ptr, node_idx]() {
                run_task(graph, counters_ptr, node_idx);
            });
        }

        m_pool.wait();

        if (m_profiler_callback) {
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> diff = end - start;
            m_profiler_callback("Executor::run", diff.count());
        }
    }

    void Executor::run_task(const ExecutionGraph &graph, std::atomic<int> *counters, int node_idx) {
        // Run user work
        if (graph.nodes[node_idx].work) {
            graph.nodes[node_idx].work();
        }

        // Decrement dependents
        for (int dep_idx : graph.nodes[node_idx].dependents) {
            // fetch_sub returns PREVIOUS value.
            // If prev was 1, it becomes 0, meaning dependencies are met.
            int prev = counters[dep_idx].fetch_sub(1, std::memory_order_release);
            if (prev == 1) {
                // Ensure visibility
                std::atomic_thread_fence(std::memory_order_acquire);
                m_pool.enqueue(
                    [this, &graph, counters, dep_idx]() { run_task(graph, counters, dep_idx); });
            }
        }
    }

}
