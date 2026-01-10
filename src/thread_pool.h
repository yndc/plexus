#pragma once
#include "work_stealing_queue.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Plexus {

    // Thread-local worker context
    inline thread_local size_t t_worker_index = SIZE_MAX;
    inline thread_local size_t t_worker_count = 0;

    /**
     * @brief A high-performance, work-stealing thread pool.
     *
     * Features:
     * - **Lock-Free Per-Worker Queues**: Each worker has a `WorkStealingQueue` (Chase-Lev
     * algorithm) for lock-free local operations.
     * - **Work Stealing**: Idle threads steal work from other threads using the lock-free
     * `steal()` method.
     * - **Central Overflow Queue**: When worker queues are full, tasks spill to a mutex-protected
     * central queue.
     * - **LIFO Scheduling**: Local workers pop from the back (LIFO) for better cache locality.
     */
    class ThreadPool {
    public:
        // Fixed storage size to accommodate Executor's capture (approx 28-32 bytes) +
        // vtable/overhead
        static constexpr size_t kTaskStorageSize = 64;

        // Custom Type Erasure for fixed-size storage to avoid heap allocations
        class FixedFunction {
        public:
            FixedFunction() = default;

            template <typename F> FixedFunction(F &&f) {
                static_assert(sizeof(F) <= kTaskStorageSize, "Task too large for FixedFunction");
                static_assert(std::is_trivially_copyable_v<F> || std::is_move_constructible_v<F>,
                              "Task must be movable");

                new (m_storage) F(std::forward<F>(f));
                m_invoke = [](void *storage) { (*reinterpret_cast<F *>(storage))(); };
                m_dtor = [](void *storage) { reinterpret_cast<F *>(storage)->~F(); };
                m_move = [](void *dest, void *src) {
                    new (dest) F(std::move(*reinterpret_cast<F *>(src)));
                };
            }

            FixedFunction(FixedFunction &&other) noexcept {
                if (other.m_invoke) {
                    other.m_move(m_storage, other.m_storage);
                    m_invoke = other.m_invoke;
                    m_dtor = other.m_dtor;
                    m_move = other.m_move;
                    other.m_invoke = nullptr; // Mark source as empty
                }
            }

            FixedFunction &operator=(FixedFunction &&other) noexcept {
                if (this != &other) {
                    if (m_invoke)
                        m_dtor(m_storage);
                    if (other.m_invoke) {
                        other.m_move(m_storage, other.m_storage);
                        m_invoke = other.m_invoke;
                        m_dtor = other.m_dtor;
                        m_move = other.m_move;
                        other.m_invoke = nullptr;
                    } else {
                        m_invoke = nullptr;
                    }
                }
                return *this;
            }

            ~FixedFunction() {
                if (m_invoke)
                    m_dtor(m_storage);
            }

            void operator()() {
                if (m_invoke)
                    m_invoke(m_storage);
            }

            explicit operator bool() const { return m_invoke != nullptr; }

        private:
            alignas(std::max_align_t) std::byte m_storage[kTaskStorageSize];
            void (*m_invoke)(void *) = nullptr;
            void (*m_dtor)(void *) = nullptr;
            void (*m_move)(void *dest, void *src) = nullptr;
        };

        using Task = FixedFunction;

        ThreadPool(int num_threads = 0) {
            unsigned int count = num_threads ? num_threads : std::thread::hardware_concurrency();
            if (count == 0)
                count = 2;
            if (count > 1)
                count--;

            m_queues.reserve(count);
            for (unsigned int i = 0; i < count; ++i) {
                m_queues.push_back(std::make_unique<WorkQueue>());
            }
            // Threads must be started after queues are initialized
            for (unsigned int i = 0; i < count; ++i) {
                m_threads.emplace_back(&ThreadPool::worker_thread, this, i);
            }
        }

        ~ThreadPool() {
            m_stop.store(true, std::memory_order_relaxed);
            // Wake all workers using per-worker CVs
            for (auto &q : m_queues) {
                std::lock_guard<std::mutex> lock(q->mutex);
                q->cv.notify_one();
            }
            for (auto &t : m_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }

        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        void dispatch(std::vector<Task> &&tasks) {
            if (tasks.empty())
                return;

            // Atomic increment
            m_active_tasks.fetch_add(static_cast<int>(tasks.size()), std::memory_order_relaxed);
            m_queued_tasks.fetch_add(static_cast<int>(tasks.size()), std::memory_order_relaxed);

            if (t_worker_index < m_queues.size()) {
                // Worker thread: push to OWN queue (lock-free, single-owner)
                size_t worker_idx = t_worker_index;
                for (auto &task : tasks) {
                    if (!m_queues[worker_idx]->queue.push(std::move(task))) {
                        // Queue full, spill to central
                        std::lock_guard<std::mutex> lock(m_overflow_mutex);
                        m_overflow_queue.push_back(std::move(task));
                    }
                }
            } else {
                // External thread: push all to central queue (mutex-protected)
                std::lock_guard<std::mutex> lock(m_overflow_mutex);
                for (auto &task : tasks) {
                    m_overflow_queue.push_back(std::move(task));
                }
            }
            // Smart wake-up: wake specific workers using per-worker CVs
            size_t tasks_count = tasks.size();
            size_t workers_woken = 0;
            for (size_t i = 0; i < m_queues.size() && workers_woken < tasks_count; ++i) {
                if (m_queues[i]->sleeping.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard<std::mutex> lock(m_queues[i]->mutex);
                    }
                    m_queues[i]->cv.notify_one();
                    ++workers_woken;
                }
            }
        }

        template <typename F> void enqueue(F &&f) {
            if (m_stop.load(std::memory_order_relaxed))
                throw std::runtime_error("ThreadPool stopped");

            m_active_tasks.fetch_add(1, std::memory_order_relaxed);
            m_queued_tasks.fetch_add(1, std::memory_order_relaxed);

            Task task(std::forward<F>(f));

            if (t_worker_index < m_queues.size()) {
                // Worker thread: push to OWN queue (lock-free, single-owner)
                if (!m_queues[t_worker_index]->queue.push(std::move(task))) {
                    // Queue full, spill to central
                    std::lock_guard<std::mutex> lock(m_overflow_mutex);
                    m_overflow_queue.push_back(std::move(task));
                }
            } else {
                // External thread: push to central queue (mutex-protected)
                std::lock_guard<std::mutex> lock(m_overflow_mutex);
                m_overflow_queue.push_back(std::move(task));
            }
            // Wake one sleeping worker using per-worker CV
            for (size_t i = 0; i < m_queues.size(); ++i) {
                if (m_queues[i]->sleeping.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard<std::mutex> lock(m_queues[i]->mutex);
                    }
                    m_queues[i]->cv.notify_one();
                    break;
                }
            }
        }

        void wait() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_done.wait(
                lock, [this]() { return m_active_tasks.load(std::memory_order_relaxed) == 0; });
        }

        void reserve_task_capacity(size_t capacity) {
            // WorkStealingQueue has fixed capacity, no-op
            (void)capacity;
        }

        /**
         * @brief Returns the number of worker threads.
         */
        size_t worker_count() const { return m_queues.size(); }

    private:
        struct alignas(64) WorkQueue {
            WorkStealingQueue<Task> queue;
            std::mutex mutex;                  // Per-worker mutex for CV
            std::condition_variable cv;        // Per-worker condition variable
            std::atomic<bool> sleeping{false}; // Is this worker sleeping?

            explicit WorkQueue(std::size_t capacity = 4096) : queue(capacity) {}
        };

        std::vector<std::unique_ptr<WorkQueue>> m_queues;
        std::vector<std::thread> m_threads;

        // Central overflow queue for when worker queues are full
        std::mutex m_overflow_mutex;
        std::deque<Task> m_overflow_queue;

        // Global synchronization for completion and stopping
        std::mutex m_mutex;
        std::condition_variable m_cv_done;
        std::atomic<bool> m_stop{false};
        std::atomic<int> m_active_tasks{0};
        std::atomic<int> m_queued_tasks{0};

        void worker_thread(int index) {
            t_worker_index = static_cast<size_t>(index);
            t_worker_count = m_queues.size();
            const size_t queue_count = m_queues.size();
            int local_task_count = 0; // Track tasks locally for batch decrement

            while (true) {
                std::optional<Task> task_opt;

                // 1. Try local queue (LIFO for cache locality) - LOCK-FREE
                task_opt = m_queues[index]->queue.pop();
                if (!task_opt.has_value()) {
                    // 2. Steal from other worker queues - LOCK-FREE
                    for (size_t i = 0; !task_opt.has_value() && i < queue_count - 1; ++i) {
                        size_t steal_idx = (index + i + 1) % queue_count;
                        task_opt = m_queues[steal_idx]->queue.steal();
                    }
                }

                // 3. Try central overflow queue if we still don't have a task
                // Batch-grab: take half of available tasks to reduce contention and enable stealing
                if (!task_opt.has_value()) {
                    std::unique_lock<std::mutex> lock(m_overflow_mutex, std::try_to_lock);
                    if (lock && !m_overflow_queue.empty()) {
                        // Take one task for immediate execution
                        task_opt = std::move(m_overflow_queue.front());
                        m_overflow_queue.pop_front();

                        // Batch-grab: take up to half of remaining tasks for local queue
                        // Cap at a reasonable maximum to avoid one worker hoarding everything
                        size_t remaining = m_overflow_queue.size();
                        size_t to_grab = std::min(remaining / 2, size_t{64});

                        for (size_t i = 0; i < to_grab; ++i) {
                            if (!m_queues[index]->queue.push(std::move(m_overflow_queue.front()))) {
                                break; // Local queue full
                            }
                            m_overflow_queue.pop_front();
                        }
                    }
                }

                // 4. Exponential backoff spin-wait before blocking
                if (!task_opt.has_value()) {
                    constexpr int max_spins = 64;
                    for (int spin = 1; spin <= max_spins; spin *= 2) {
                        // Quick check local queue
                        task_opt = m_queues[index]->queue.pop();
                        if (task_opt.has_value()) {
                            break;
                        }

                        // Quick check overflow queue with batch-grab
                        {
                            std::unique_lock<std::mutex> lock(m_overflow_mutex, std::try_to_lock);
                            if (lock && !m_overflow_queue.empty()) {
                                task_opt = std::move(m_overflow_queue.front());
                                m_overflow_queue.pop_front();

                                // Batch-grab remaining tasks
                                size_t remaining = m_overflow_queue.size();
                                size_t to_grab = std::min(remaining / 2, size_t{64});
                                for (size_t i = 0; i < to_grab; ++i) {
                                    if (!m_queues[index]->queue.push(
                                            std::move(m_overflow_queue.front()))) {
                                        break;
                                    }
                                    m_overflow_queue.pop_front();
                                }
                                break;
                            }
                        }

                        // Backoff: yield multiple times based on spin iteration
                        for (int y = 0; y < spin; ++y) {
                            std::this_thread::yield();
                        }
                    }
                }

                // 5. Wait for work (blocking) - use per-worker CV
                if (!task_opt.has_value()) {
                    // Batch decrement: update counter once before sleeping
                    if (local_task_count > 0) {
                        m_queued_tasks.fetch_sub(local_task_count, std::memory_order_relaxed);
                        local_task_count = 0;
                    }

                    std::unique_lock<std::mutex> lock(m_queues[index]->mutex);
                    m_queues[index]->sleeping.store(true, std::memory_order_relaxed);
                    m_queues[index]->cv.wait(lock, [this]() {
                        return m_stop.load(std::memory_order_relaxed) ||
                               m_queued_tasks.load(std::memory_order_relaxed) > 0;
                    });
                    m_queues[index]->sleeping.store(false, std::memory_order_relaxed);

                    if (m_stop.load(std::memory_order_relaxed))
                        return;

                    continue;
                }

                // Track task locally (will batch decrement later)
                ++local_task_count;

                // Execute task (we know task_opt.has_value() is true here)
                assert(task_opt.has_value() && "task_opt must have value before execution");
                try {
                    (*task_opt)();
                } catch (...) {
                    // Task threw
                }

                int prev = m_active_tasks.fetch_sub(1, std::memory_order_release);
                if (prev == 1) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_cv_done.notify_all();
                }
            }
        }
    };
}
