#pragma once
#include "work_stealing_queue.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Plexus {

    inline thread_local int t_worker_index = -1;

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
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv_work.notify_all();
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

            if (t_worker_index >= 0 && static_cast<size_t>(t_worker_index) < m_queues.size()) {
                // Worker thread: push to OWN queue (lock-free, single-owner)
                size_t worker_idx = static_cast<size_t>(t_worker_index);
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
            m_cv_work.notify_all();
        }

        template <typename F> void enqueue(F &&f) {
            if (m_stop.load(std::memory_order_relaxed))
                throw std::runtime_error("ThreadPool stopped");

            m_active_tasks.fetch_add(1, std::memory_order_relaxed);
            m_queued_tasks.fetch_add(1, std::memory_order_relaxed);

            Task task(std::forward<F>(f));

            if (t_worker_index >= 0 && static_cast<size_t>(t_worker_index) < m_queues.size()) {
                // Worker thread: push to OWN queue (lock-free, single-owner)
                if (!m_queues[static_cast<size_t>(t_worker_index)]->queue.push(std::move(task))) {
                    // Queue full, spill to central
                    std::lock_guard<std::mutex> lock(m_overflow_mutex);
                    m_overflow_queue.push_back(std::move(task));
                }
            } else {
                // External thread: push to central queue (mutex-protected)
                std::lock_guard<std::mutex> lock(m_overflow_mutex);
                m_overflow_queue.push_back(std::move(task));
            }
            m_cv_work.notify_one();
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

    private:
        struct alignas(64) WorkQueue {
            WorkStealingQueue<Task> queue;

            explicit WorkQueue(std::size_t capacity = 4096) : queue(capacity) {}
        };

        std::vector<std::unique_ptr<WorkQueue>> m_queues;
        std::vector<std::thread> m_threads;

        // Central overflow queue for when worker queues are full
        std::mutex m_overflow_mutex;
        std::deque<Task> m_overflow_queue;

        // Global synchronization for completion and stopping
        std::mutex m_mutex;
        std::condition_variable m_cv_work;
        std::condition_variable m_cv_done;
        std::atomic<bool> m_stop{false};
        std::atomic<int> m_active_tasks{0};
        std::atomic<int> m_queued_tasks{0};

        void worker_thread(int index) {
            t_worker_index = index;
            const size_t queue_count = m_queues.size();

            while (true) {
                std::optional<Task> task_opt;

                // 1. Try local queue (LIFO for cache locality) - LOCK-FREE
                task_opt = m_queues[index]->queue.pop();
                if (task_opt.has_value()) {
                    m_queued_tasks.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    // 2. Steal from other worker queues - LOCK-FREE
                    for (size_t i = 0; !task_opt.has_value() && i < queue_count - 1; ++i) {
                        size_t steal_idx = (index + i + 1) % queue_count;
                        task_opt = m_queues[steal_idx]->queue.steal();
                        if (task_opt.has_value()) {
                            m_queued_tasks.fetch_sub(1, std::memory_order_relaxed);
                        }
                    }
                }

                // 3. Try central overflow queue if we still don't have a task
                if (!task_opt.has_value()) {
                    std::unique_lock<std::mutex> lock(m_overflow_mutex, std::try_to_lock);
                    if (lock && !m_overflow_queue.empty()) {
                        task_opt = std::move(m_overflow_queue.front());
                        m_overflow_queue.pop_front();
                        m_queued_tasks.fetch_sub(1, std::memory_order_relaxed);
                    }
                }

                // 5. Wait for work
                if (!task_opt.has_value()) {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv_work.wait(lock, [this]() {
                        return m_stop.load(std::memory_order_relaxed) ||
                               m_queued_tasks.load(std::memory_order_relaxed) > 0;
                    });

                    if (m_stop.load(std::memory_order_relaxed))
                        return;

                    continue;
                }

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
