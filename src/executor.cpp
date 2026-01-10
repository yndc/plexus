#include "plexus/executor.h"
#include "thread_pool.h"
#include <iostream>
#include <memory>

namespace Plexus {

    // ========================================================================
    // AsyncHandle::State
    // ========================================================================

    struct AsyncHandle::State {
        std::atomic<bool> done{false};
        std::atomic<bool> cancel_execution{false};

        std::mutex mutex;
        std::condition_variable cv;

        std::mutex exception_mutex;
        std::vector<std::exception_ptr> exceptions;

        std::atomic<int> active_task_count{0};

        // For main thread affinity support
        std::mutex main_queue_mutex;
        std::condition_variable cv_main_thread;
        std::vector<int> main_queue_backlog;

        // Atomic counters for dependencies
        std::unique_ptr<std::atomic<int>[]> counters;
        size_t counter_size = 0;

        void signal_done() {
            done.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    };

    // ========================================================================
    // AsyncHandle
    // ========================================================================

    AsyncHandle::AsyncHandle(std::shared_ptr<State> state) : m_state(std::move(state)) {}

    void AsyncHandle::wait() {
        if (!m_state)
            return;

        std::unique_lock<std::mutex> lock(m_state->mutex);
        m_state->cv.wait(lock, [this]() { return m_state->done.load(std::memory_order_acquire); });

        // Rethrow first exception
        std::lock_guard<std::mutex> ex_lock(m_state->exception_mutex);
        if (!m_state->exceptions.empty()) {
            std::rethrow_exception(m_state->exceptions.front());
        }
    }

    bool AsyncHandle::is_done() const {
        return m_state ? m_state->done.load(std::memory_order_acquire) : true;
    }

    const std::vector<std::exception_ptr> &AsyncHandle::get_exceptions() const {
        static const std::vector<std::exception_ptr> empty;
        if (!m_state)
            return empty;
        return m_state->exceptions;
    }

    // ========================================================================
    // Executor
    // ========================================================================

    Executor::Executor()
        : m_owned_pool(std::make_unique<ThreadPool>()), m_pool(m_owned_pool.get()) {}

    Executor::Executor(ThreadPool &pool) : m_pool(&pool) {}

    Executor::~Executor() = default;

    void Executor::run(const ExecutionGraph &graph, ExecutionMode mode) {
        run_async(graph, mode).wait();
    }

    AsyncHandle Executor::run_async(const ExecutionGraph &graph, ExecutionMode mode) {
        auto state = std::make_shared<AsyncHandle::State>();

        if (graph.nodes.empty()) {
            state->signal_done();
            return AsyncHandle(state);
        }

        if (mode == ExecutionMode::Sequential) {
            run_sequential(graph, state);
            return AsyncHandle(state);
        }

        run_parallel(graph, state);
        return AsyncHandle(state);
    }

    void Executor::run_parallel(const ExecutionGraph &graph,
                                std::shared_ptr<AsyncHandle::State> async_state) {
        // 0. Pre-allocate Thread Pool Ring Buffers
        m_pool->reserve_task_capacity(graph.nodes.size());

        // 1. Initialize State
        async_state->counters.reset(new std::atomic<int>[graph.nodes.size()]);
        async_state->counter_size = graph.nodes.size();

        std::atomic<int> *counters_ptr = async_state->counters.get();

        // Check if any node requires main thread affinity
        bool has_main_thread_tasks = false;
        for (const auto &node : graph.nodes) {
            if (node.thread_affinity == ThreadAffinity::Main) {
                has_main_thread_tasks = true;
                break;
            }
        }

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            counters_ptr[i].store(graph.nodes[i].initial_dependencies, std::memory_order_relaxed);
        }

        // Initialize active count with entry nodes
        async_state->active_task_count.store(static_cast<int>(graph.entry_nodes.size()),
                                             std::memory_order_relaxed);

        // 2. Submit Entry Nodes
        for (int node_idx : graph.entry_nodes) {
            if (graph.nodes[node_idx].thread_affinity == ThreadAffinity::Main) {
                {
                    std::lock_guard<std::mutex> lock(async_state->main_queue_mutex);
                    async_state->main_queue_backlog.push_back(node_idx);
                }
            } else {
                m_pool->enqueue([this, &graph, counters_ptr, async_state, node_idx]() {
                    run_task(graph, counters_ptr, node_idx, async_state);
                });
            }
        }

        // 3. Waiter Logic
        if (!has_main_thread_tasks) {
            // No main thread affinity tasks - spawn a waiter thread for true async
            m_pool->enqueue([async_state, profiler = m_profiler_callback,
                             start = std::chrono::high_resolution_clock::now()]() {
                // Wait for all tasks to complete
                {
                    std::unique_lock<std::mutex> lock(async_state->main_queue_mutex);
                    async_state->cv_main_thread.wait(lock, [&]() {
                        return async_state->active_task_count.load(std::memory_order_acquire) == 0;
                    });
                }

                if (profiler) {
                    auto end = std::chrono::high_resolution_clock::now();
                    std::chrono::duration<double, std::milli> diff = end - start;
                    profiler("Executor::run", diff.count());
                }

                async_state->signal_done();
            });
        } else {
            // Has main thread affinity tasks - run the main thread loop synchronously
            auto start = std::chrono::high_resolution_clock::now();

            // Set main thread as designated worker index = worker_count()
            size_t prev_index = t_worker_index;
            size_t prev_count = t_worker_count;
            t_worker_index = m_pool->worker_count(); // Designated main thread index
            t_worker_count = m_pool->worker_count();

            struct ContextGuard {
                size_t &idx, &cnt;
                size_t prev_idx, prev_cnt;
                ~ContextGuard() {
                    idx = prev_idx;
                    cnt = prev_cnt;
                }
            } guard{t_worker_index, t_worker_count, prev_index, prev_count};

            while (async_state->active_task_count.load(std::memory_order_acquire) > 0) {
                // Drain Main Queue
                std::vector<int> local_main_tasks;
                {
                    std::unique_lock<std::mutex> lock(async_state->main_queue_mutex);
                    if (!async_state->main_queue_backlog.empty()) {
                        local_main_tasks.swap(async_state->main_queue_backlog);
                    }
                }

                if (!local_main_tasks.empty()) {
                    for (int node_idx : local_main_tasks) {
                        run_task(graph, counters_ptr, node_idx, async_state);
                    }
                } else {
                    // Wait for signal (New Main Task OR Graph Done)
                    std::unique_lock<std::mutex> lock(async_state->main_queue_mutex);
                    async_state->cv_main_thread.wait(lock, [&]() {
                        return !async_state->main_queue_backlog.empty() ||
                               async_state->active_task_count.load(std::memory_order_acquire) == 0;
                    });
                }
            }

            if (m_profiler_callback) {
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> diff = end - start;
                m_profiler_callback("Executor::run", diff.count());
            }

            async_state->signal_done();
        }
    }

    void Executor::run_sequential(const ExecutionGraph &graph,
                                  std::shared_ptr<AsyncHandle::State> async_state) {
        auto start = std::chrono::high_resolution_clock::now();

        // Set worker context for sequential mode (single logical worker)
        size_t prev_index = t_worker_index;
        size_t prev_count = t_worker_count;
        t_worker_index = 0;
        t_worker_count = 1;

        struct ContextGuard {
            size_t &idx, &cnt;
            size_t prev_idx, prev_cnt;
            ~ContextGuard() {
                idx = prev_idx;
                cnt = prev_cnt;
            }
        } guard{t_worker_index, t_worker_count, prev_index, prev_count};

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
                std::lock_guard<std::mutex> lock(async_state->exception_mutex);
                async_state->exceptions.push_back(std::current_exception());

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

        async_state->signal_done();
    }

    void Executor::run_task(const ExecutionGraph &graph, std::atomic<int> *counters, int node_idx,
                            std::shared_ptr<AsyncHandle::State> async_state) {
        // RAII to ensure the current task is always decremented on exit
        struct DoneGuard {
            std::shared_ptr<AsyncHandle::State> state;
            DoneGuard(std::shared_ptr<AsyncHandle::State> s) : state(std::move(s)) {}
            ~DoneGuard() {
                // If we are unwinding due to an uncaught exception (e.g. enqueue failed),
                // we should signal cancellation if not already done.
                if (std::uncaught_exceptions() > 0) {
                    state->cancel_execution.store(true, std::memory_order_relaxed);
                }

                int remaining =
                    state->active_task_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0 || state->cancel_execution.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> lock(state->main_queue_mutex);
                    state->cv_main_thread.notify_all();
                }
            }
        } done_guard(async_state);

        bool skip_dependents = false;

        // 1. Run User Work
        try {
            if (graph.nodes[node_idx].work) {
                graph.nodes[node_idx].work();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(async_state->exception_mutex);
            async_state->exceptions.push_back(std::current_exception());

            auto policy = graph.nodes[node_idx].error_policy;

            if (policy == ErrorPolicy::CancelGraph) {
                async_state->cancel_execution.store(true, std::memory_order_relaxed);
                // DoneGuard will handle decrement and notify
                return;
            } else if (policy == ErrorPolicy::CancelDependents) {
                skip_dependents = true;
            }
        }

        // 2. Check Global Cancellation
        if (async_state->cancel_execution.load(std::memory_order_relaxed)) {
            return;
        }

        // 3. Trigger Dependents
        if (!skip_dependents) {
            for (int dep_idx : graph.nodes[node_idx].dependents) {
                // Use acq_rel: release for publishing our completion, acquire for reading dep_idx
                // node data
                int prev = counters[dep_idx].fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    if (async_state->cancel_execution.load(std::memory_order_relaxed))
                        break;

                    // Increment active count BEFORE enqueueing
                    // This is necessary because the task might start executing immediately after
                    // enqueue
                    async_state->active_task_count.fetch_add(1, std::memory_order_relaxed);

                    try {
                        if (graph.nodes[dep_idx].thread_affinity == ThreadAffinity::Main) {
                            {
                                std::lock_guard<std::mutex> lock(async_state->main_queue_mutex);
                                async_state->main_queue_backlog.push_back(dep_idx);
                            }
                            async_state->cv_main_thread.notify_one();
                        } else {
                            m_pool->enqueue([this, &graph, counters, async_state, dep_idx]() {
                                run_task(graph, counters, dep_idx, async_state);
                            });
                        }
                    } catch (...) {
                        // Rollback count for the child we failed to enqueue
                        int remaining =
                            async_state->active_task_count.fetch_sub(1, std::memory_order_relaxed) -
                            1;

                        // Signal failure
                        async_state->cancel_execution.store(true, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lock(async_state->exception_mutex);
                            async_state->exceptions.push_back(std::current_exception());
                        }

                        // CRITICAL: Notify main thread if count reached zero
                        // This prevents main thread from waiting forever if the rollback brought
                        // count to 0
                        if (remaining == 0) {
                            std::lock_guard<std::mutex> lock(async_state->main_queue_mutex);
                            async_state->cv_main_thread.notify_all();
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
