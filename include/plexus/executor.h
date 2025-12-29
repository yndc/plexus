#pragma once
#include "plexus/execution_graph.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace Plexus {

    class ThreadPool;

    /**
     * @brief Execution mode for the Executor.
     */
    enum class ExecutionMode {
        Parallel,  ///< Use thread pool for parallel execution (default)
        Sequential ///< Run all tasks on the calling thread (for debugging)
    };

    /**
     * @brief A generic executor for processing ExecutionGraphs.
     *
     * The Executor takes a static ExecutionGraph and processes it dynamically using
     * the provided ThreadPool. It blocks until the entire graph has finished execution.
     */
    class Executor {
    public:
        /**
         * @brief Constructs an Executor with a default internal ThreadPool.
         * The internal pool uses (hardware_concurrency - 1) threads.
         */
        Executor();

        /**
         * @brief Constructs an Executor using an externally managed ThreadPool.
         * @param pool The thread pool to use for task execution.
         */
        explicit Executor(ThreadPool &pool);

        /**
         * @brief Destructor.
         */
        ~Executor();

        /**
         * @brief Executes the given graph.
         *
         * This function blocks the calling thread until all tasks in the graph have
         * completed.
         *
         * @param graph The dependency graph to execute.
         * @param mode Execution mode. Use Sequential for single-threaded debugging.
         */
        void run(const ExecutionGraph &graph, ExecutionMode mode = ExecutionMode::Parallel);

        /**
         * @brief Callback for profiling.
         * @param name Optional label.
         * @param duration_ms Duration in milliseconds.
         */
        using ProfilerCallback = std::function<void(const char *name, double duration_ms)>;

        void set_profiler_callback(ProfilerCallback callback) {
            m_profiler_callback = std::move(callback);
        }

    private:
        void run_task(const ExecutionGraph &graph, std::atomic<int> *counters, int node_idx);
        void run_sequential(const ExecutionGraph &graph);

        std::unique_ptr<ThreadPool> m_owned_pool;
        ThreadPool *m_pool;
        ProfilerCallback m_profiler_callback;

        // Error Handling
        std::atomic<bool> m_cancel_graph_execution{false};
        std::mutex m_exception_mutex;
        std::vector<std::exception_ptr> m_exceptions;

        // Zero-Allocation Cache
        std::unique_ptr<std::atomic<int>[]> m_counter_cache;
        size_t m_counter_cache_size = 0;

        // Thread Affinity Support
        void process_main_thread_tasks();
        std::vector<int> m_main_thread_queue;
        std::mutex m_main_queue_mutex;
        std::vector<int> m_main_queue_backlog; // Incoming tasks for main thread

        std::atomic<int> m_active_task_count{0};
        std::condition_variable m_cv_main_thread; // To wake up main thread
    };
}
