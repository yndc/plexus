#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Plexus {

    /**
     * @brief A basic thread pool for executing tasks in parallel.
     *
     * Uses a fixed number of worker threads (std::thread::hardware_concurrency - 1)
     * consuming from a shared blocking queue. Supports a bulk dispatch and a
     * barrier wait mechanism.
     */
    class ThreadPool {
    public:
        ThreadPool();
        ~ThreadPool();

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        using Task = std::function<void()>;

        /**
         * @brief Dispatches a batch of tasks to the worker queue.
         *
         * This function is thread-safe.
         *
         * @param tasks A vector of void() functions to execute.
         */
        void dispatch(const std::vector<Task> &tasks);

        /**
         * @brief Enqueues a single task.
         *
         * Thread-safe and efficient for runtime dynamic scheduling.
         *
         * @param task The void() function to execute.
         */
        void enqueue(Task task);

        /**
         * @brief Blocks the calling thread until all currently active (dispatched)
         * tasks are complete.
         *
         * This acts as a barrier synchronization point.
         */
        void wait();

    private:
        void worker_thread();

        std::vector<std::thread> m_workers;
        std::queue<Task> m_queue;

        std::mutex m_mutex;
        std::condition_variable m_cv_work; // Notify workers of new tasks
        std::condition_variable m_cv_done; // Notify main thread when tasks complete

        std::atomic<bool> m_stop = false;
        std::atomic<int> m_active_tasks = 0; // Tasks currently in queue or running
    };

}
