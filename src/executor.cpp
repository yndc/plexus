#include "plexus/executor.h"
#include "thread_pool.h"
#include <iostream>
#include <memory>

namespace Plexus {

    Executor::Executor()
        : m_owned_pool(std::make_unique<ThreadPool>()), m_pool(m_owned_pool.get()) {}

    Executor::Executor(ThreadPool &pool) : m_pool(&pool) {}

    Executor::~Executor() = default;

    void Executor::run(const ExecutionGraph &graph) {
        if (graph.nodes.empty())
            return;

        auto start = std::chrono::high_resolution_clock::now();

        // 0. Pre-allocate Thread Pool Ring Buffers
        m_pool->reserve_task_capacity(graph.nodes.size());

        // Reset State
        m_cancel_graph_execution = false;
        {
            std::lock_guard<std::mutex> lock(m_exception_mutex);
            m_exceptions.clear();
        }

        // 1. Initialize State
        // Zero-Allocation: Reuse atomic counters if capacity allows
        if (m_counter_cache_size < graph.nodes.size()) {
            m_counter_cache.reset(new std::atomic<int>[graph.nodes.size()]);
            m_counter_cache_size = graph.nodes.size();
        }

        std::atomic<int> *counters_ptr = m_counter_cache.get();

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            counters_ptr[i].store(graph.nodes[i].initial_dependencies, std::memory_order_relaxed);
        }

        // Initialize active count with entry nodes
        m_active_task_count = static_cast<int>(graph.entry_nodes.size());

        // 2. Submit Entry Nodes
        for (int node_idx : graph.entry_nodes) {
            if (graph.nodes[node_idx].thread_affinity == ThreadAffinity::Main) {
                {
                    std::lock_guard<std::mutex> lock(m_main_queue_mutex);
                    m_main_queue_backlog.push_back(node_idx);
                }
            } else {
                m_pool->enqueue([this, &graph, counters_ptr,
                                 node_idx]() { run_task(graph, counters_ptr, node_idx); },
                                graph.nodes[node_idx].priority);
            }
        }

        // 3. Main Thread Loop (Wait for completion)
        // We continue until all active tasks are drained, even if cancelled.
        while (m_active_task_count > 0) {
            // 3a. Drain Main Queue
            std::vector<int> local_main_tasks;
            {
                std::unique_lock<std::mutex> lock(m_main_queue_mutex);
                if (!m_main_queue_backlog.empty()) {
                    local_main_tasks.swap(m_main_queue_backlog);
                }
            }

            if (!local_main_tasks.empty()) {
                for (int node_idx : local_main_tasks) {
                    run_task(graph, counters_ptr, node_idx);
                }
            } else {
                // Wait for signal (New Main Task OR Graph Done)
                std::unique_lock<std::mutex> lock(m_main_queue_mutex);
                m_cv_main_thread.wait(lock, [&]() {
                    return !m_main_queue_backlog.empty() || m_active_task_count == 0;
                });
            }
        }

        if (m_profiler_callback) {
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> diff = end - start;
            m_profiler_callback("Executor::run", diff.count());
        }

        // Rethrow exceptions
        std::lock_guard<std::mutex> lock(m_exception_mutex);
        if (!m_exceptions.empty()) {
            std::rethrow_exception(m_exceptions.front());
        }
    }

    void Executor::run_task(const ExecutionGraph &graph, std::atomic<int> *counters, int node_idx) {
        bool skip_dependents = false;

        // 1. Run User Work
        try {
            if (graph.nodes[node_idx].work) {
                graph.nodes[node_idx].work();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_exception_mutex);
            m_exceptions.push_back(std::current_exception());

            auto policy = graph.nodes[node_idx].error_policy;

            if (policy == ErrorPolicy::CancelGraph) {
                m_cancel_graph_execution = true;
                // Decrement active count since we are stopping
                m_active_task_count.fetch_sub(1);
                m_cv_main_thread.notify_all();
                return;
            } else if (policy == ErrorPolicy::CancelDependents) {
                skip_dependents = true;
            }
        }

        // 2. Check Global Cancellation
        if (m_cancel_graph_execution) {
            m_active_task_count.fetch_sub(1);
            m_cv_main_thread.notify_all();
            return;
        }

        // 3. Trigger Dependents
        if (!skip_dependents) {
            for (int dep_idx : graph.nodes[node_idx].dependents) {
                int prev = counters[dep_idx].fetch_sub(1, std::memory_order_release);
                if (prev == 1) {
                    if (m_cancel_graph_execution)
                        break;

                    std::atomic_thread_fence(std::memory_order_acquire);

                    // Increment active count BEFORE enqueueing
                    m_active_task_count.fetch_add(1);

                    if (graph.nodes[dep_idx].thread_affinity == ThreadAffinity::Main) {
                        {
                            std::lock_guard<std::mutex> lock(m_main_queue_mutex);
                            m_main_queue_backlog.push_back(dep_idx);
                        }
                        m_cv_main_thread.notify_one();
                    } else {
                        m_pool->enqueue([this, &graph, counters,
                                         dep_idx]() { run_task(graph, counters, dep_idx); },
                                        graph.nodes[dep_idx].priority);
                    }
                }
            }
        }

        // 4. Task Complete
        int remaining = m_active_task_count.fetch_sub(1) - 1;
        if (remaining == 0) {
            m_cv_main_thread.notify_all();
        }
    }

} // namespace Plexus
