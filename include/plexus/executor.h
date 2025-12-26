#pragma once
#include "plexus/execution_graph.h"
#include "plexus/thread_pool.h"
#include <atomic>

namespace Plexus {

    /**
     * @brief A generic executor for processing ExecutionGraphs.
     *
     * The Executor takes a static ExecutionGraph and processes it dynamically using
     * the provided ThreadPool. It blocks until the entire graph has finished execution.
     */
    class Executor {
    public:
        explicit Executor(ThreadPool &pool);

        /**
         * @brief Executes the given graph.
         *
         * This function blocks the calling thread until all tasks in the graph have
         * completed.
         *
         * @param graph The dependency graph to execute.
         */
        void run(const ExecutionGraph &graph);

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

        ThreadPool &m_pool;
        ProfilerCallback m_profiler_callback;

        // Error Handling
        std::atomic<bool> m_cancel_graph_execution{false};
        std::mutex m_exception_mutex;
        std::vector<std::exception_ptr> m_exceptions;
    };

}
