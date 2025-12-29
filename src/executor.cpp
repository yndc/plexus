#include "plexus/executor.h"
#include "thread_pool.h"
#include <iostream>
#include <memory>

namespace Plexus {

    Executor::Executor()
        : m_owned_pool(std::make_unique<ThreadPool>()), m_pool(m_owned_pool.get()) {}

    Executor::Executor(ThreadPool &pool) : m_pool(&pool) {}

    Executor::~Executor() = default;

    void Executor::run(const ExecutionGraph &graph, ExecutionMode mode) {
        if (graph.nodes.empty())
            return;

        // Sequential mode: run all tasks on the calling thread
        if (mode == ExecutionMode::Sequential) {
            run_sequential(graph);
            return;
        }

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
                m_pool->enqueue([this, &graph, counters_ptr, node_idx]() {
                    run_task(graph, counters_ptr, node_idx);
                });
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

    void Executor::run_sequential(const ExecutionGraph &graph) {
        auto start = std::chrono::high_resolution_clock::now();

        // Reset State
        m_exceptions.clear();

        // Use simple int counters (no atomics needed for single-threaded)
        std::vector<int> counters(graph.nodes.size());
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            counters[i] = graph.nodes[i].initial_dependencies;
        }

        // Queue of ready nodes
        std::vector<int> ready_queue(graph.entry_nodes.begin(), graph.entry_nodes.end());

        while (!ready_queue.empty()) {
            int node_idx = ready_queue.back();
            ready_queue.pop_back();

            // Execute the task
            try {
                if (graph.nodes[node_idx].work) {
                    graph.nodes[node_idx].work();
                }
            } catch (...) {
                m_exceptions.push_back(std::current_exception());

                auto policy = graph.nodes[node_idx].error_policy;
                if (policy == ErrorPolicy::CancelGraph) {
                    break; // Stop execution entirely
                } else if (policy == ErrorPolicy::CancelDependents) {
                    continue; // Skip triggering dependents
                }
                // ErrorPolicy::Continue: fall through to trigger dependents
            }

            // Trigger dependents
            for (int dep_idx : graph.nodes[node_idx].dependents) {
                if (--counters[dep_idx] == 0) {
                    ready_queue.push_back(dep_idx);
                }
            }
        }

        if (m_profiler_callback) {
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> diff = end - start;
            m_profiler_callback("Executor::run_sequential", diff.count());
        }

        // Rethrow first exception
        if (!m_exceptions.empty()) {
            std::rethrow_exception(m_exceptions.front());
        }
    }

    void Executor::run_task(const ExecutionGraph &graph, std::atomic<int> *counters, int node_idx) {
        // RAII to ensure the current task is always decremented on exit
        struct DoneGuard {
            Executor &exec;
            DoneGuard(Executor &e) : exec(e) {}
            ~DoneGuard() {
                // If we are unwinding due to an uncaught exception (e.g. enqueue failed),
                // we should signal cancellation if not already done.
                if (std::uncaught_exceptions() > 0) {
                    exec.m_cancel_graph_execution = true;
                }

                int remaining = exec.m_active_task_count.fetch_sub(1) - 1;
                if (remaining == 0 || exec.m_cancel_graph_execution) {
                    std::lock_guard<std::mutex> lock(exec.m_main_queue_mutex);
                    exec.m_cv_main_thread.notify_all();
                }
            }
        } done_guard(*this);

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
                // DoneGuard will handle decrement and notify
                return;
            } else if (policy == ErrorPolicy::CancelDependents) {
                skip_dependents = true;
            }
        }

        // 2. Check Global Cancellation
        if (m_cancel_graph_execution) {
            return;
        }

        // 3. Trigger Dependents
        if (!skip_dependents) {
            for (int dep_idx : graph.nodes[node_idx].dependents) {
                // Use acq_rel: release for publishing our completion, acquire for reading dep_idx
                // node data
                int prev = counters[dep_idx].fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    if (m_cancel_graph_execution)
                        break;

                    // Increment active count BEFORE enqueueing
                    // This is necessary because the task might start executing immediately after
                    // enqueue
                    m_active_task_count.fetch_add(1, std::memory_order_relaxed);

                    try {
                        if (graph.nodes[dep_idx].thread_affinity == ThreadAffinity::Main) {
                            {
                                std::lock_guard<std::mutex> lock(m_main_queue_mutex);
                                m_main_queue_backlog.push_back(dep_idx);
                            }
                            m_cv_main_thread.notify_one();
                        } else {
                            m_pool->enqueue([this, &graph, counters, dep_idx]() {
                                run_task(graph, counters, dep_idx);
                            });
                        }
                    } catch (...) {
                        // Rollback count for the child we failed to enqueue
                        int remaining =
                            m_active_task_count.fetch_sub(1, std::memory_order_relaxed) - 1;

                        // Signal failure
                        m_cancel_graph_execution = true;
                        {
                            std::lock_guard<std::mutex> lock(m_exception_mutex);
                            m_exceptions.push_back(std::current_exception());
                        }

                        // CRITICAL: Notify main thread if count reached zero
                        // This prevents main thread from waiting forever if the rollback brought
                        // count to 0
                        if (remaining == 0) {
                            std::lock_guard<std::mutex> lock(m_main_queue_mutex);
                            m_cv_main_thread.notify_all();
                        }

                        // Graph is broken, stop processing dependents
                        break;
                    }
                }
            }
        }

        // DoneGuard destructor handles Step 4.
    }

}
