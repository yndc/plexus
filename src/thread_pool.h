#pragma once
#include "ring_buffer.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Plexus {

    inline thread_local int t_worker_index = -1;

    /**
     * @brief A simplified thread pool with a single global queue using a ring buffer.
     * Guaranteed zero-allocation if reserve_task_capacity is used appropriately.
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

            int queue_idx = 0;
            // Use thread-local index if available, otherwise round-robin
            if (t_worker_index >= 0 && static_cast<size_t>(t_worker_index) < m_queues.size()) {
                queue_idx = t_worker_index;
            }

            for (auto &task : tasks) {
                bool pushed = false;
                // Try strictly local first
                {
                    std::lock_guard<std::mutex> guard(m_queues[queue_idx]->mutex);
                    pushed = m_queues[queue_idx]->queue.push(std::move(task));
                }

                if (!pushed) {
                    // Fallback to resizing the queue (RingBuffer auto-resizes in push)
                    // The only reason push returns false is not handled by RingBuffer yet (it
                    // always returns true) But if we wanted to balance load:
                    queue_idx = (queue_idx + 1) % m_queues.size();
                    std::lock_guard<std::mutex> guard(m_queues[queue_idx]->mutex);
                    m_queues[queue_idx]->queue.push(std::move(task));
                }
            }
            // Notify all because we pushed multiple tasks
            m_cv_work.notify_all();
        }

        template <typename F> void enqueue(F &&f, int priority = 4) {
            (void)priority;

            if (m_stop.load(std::memory_order_relaxed))
                throw std::runtime_error("ThreadPool stopped");

            m_active_tasks.fetch_add(1, std::memory_order_relaxed);
            m_queued_tasks.fetch_add(1, std::memory_order_relaxed);

            // Determine target queue
            size_t target_idx = 0;
            if (t_worker_index >= 0 && static_cast<size_t>(t_worker_index) < m_queues.size()) {
                target_idx = static_cast<size_t>(t_worker_index);
            } else {
                // Round-robin for external threads
                static std::atomic<size_t> next_idx{0};
                target_idx = next_idx.fetch_add(1, std::memory_order_relaxed) % m_queues.size();
            }

            {
                std::lock_guard<std::mutex> lock(m_queues[target_idx]->mutex);
                m_queues[target_idx]->queue.push(Task(std::forward<F>(f)));
            }
            m_cv_work.notify_one();
        }

        void wait() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv_done.wait(
                lock, [this]() { return m_active_tasks.load(std::memory_order_relaxed) == 0; });
        }

        void reserve_task_capacity(size_t capacity) {
            if (m_queues.empty())
                return;
            size_t per_queue = (capacity + m_queues.size() - 1) / m_queues.size();
            // Add some slack for uneven distribution
            per_queue += 32;

            for (auto &q : m_queues) {
                std::lock_guard<std::mutex> lock(q->mutex);
                q->queue.resize(per_queue);
            }
        }

    private:
        struct alignas(64) WorkQueue {
            std::mutex mutex;
            RingBuffer<Task> queue;
        };

        std::vector<std::unique_ptr<WorkQueue>> m_queues;
        std::vector<std::thread> m_threads;

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

            // Buffer for stolen tasks
            std::vector<Task> stolen_batch;
            stolen_batch.reserve(16);

            while (true) {
                Task task;
                bool found_task = false;

                // 0. Process stolen batch first
                if (!stolen_batch.empty()) {
                    task = std::move(stolen_batch.back());
                    stolen_batch.pop_back();
                    found_task = true;
                }

                // 1. Try local queue (LIFO for cache locality)
                if (!found_task) {
                    std::lock_guard<std::mutex> lock(m_queues[index]->mutex);
                    if (m_queues[index]->queue.pop_back(task)) {
                        m_queued_tasks.fetch_sub(1, std::memory_order_relaxed);
                        found_task = true;
                    }
                }

                // 2. Steal from others
                if (!found_task && queue_count > 1) {
                    // Xorshift32 for fast thread-local random numbers
                    static thread_local uint32_t rng_state =
                        std::hash<std::thread::id>{}(std::this_thread::get_id());
                    rng_state ^= rng_state << 13;
                    rng_state ^= rng_state >> 17;
                    rng_state ^= rng_state << 5;

                    // Random start offset to prevent convoy effects
                    // We want an offset in [1, queue_count - 1]
                    size_t start_offset = (rng_state % (queue_count - 1)) + 1;

                    for (size_t i = 0; i < queue_count - 1; ++i) {
                        // Ensure we iterate through all OTHER queues exactly once
                        // shift ranges from 1 to queue_count-1
                        size_t shift = ((start_offset + i - 1) % (queue_count - 1)) + 1;
                        size_t steal_idx = (index + shift) % queue_count;

                        if (std::unique_lock<std::mutex> lock(m_queues[steal_idx]->mutex,
                                                              std::try_to_lock);
                            lock) {
                            if (!m_queues[steal_idx]->queue.empty()) {
                                // Steal up to 16 tasks
                                size_t count =
                                    m_queues[steal_idx]->queue.pop_batch(stolen_batch, 16);
                                if (count > 0) {
                                    m_queued_tasks.fetch_sub(count, std::memory_order_relaxed);
                                    task = std::move(stolen_batch.back());
                                    stolen_batch.pop_back();
                                    found_task = true;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (!found_task) {
                    // 3. Spin-Wait
                    for (int i = 0; i < 64; ++i) {
                        {
                            std::lock_guard<std::mutex> lock(m_queues[index]->mutex);
                            if (m_queues[index]->queue.pop_back(task)) {
                                m_queued_tasks.fetch_sub(1, std::memory_order_relaxed);
                                found_task = true;
                                break;
                            }
                        }
                        std::this_thread::yield();
                    }

                    // 4. Wait
                    if (!found_task) {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv_work.wait(lock, [this]() {
                            return m_stop.load(std::memory_order_relaxed) ||
                                   m_queued_tasks.load(std::memory_order_relaxed) > 0;
                        });

                        if (m_stop.load(std::memory_order_relaxed))
                            return;

                        continue;
                    }
                }

                // Execute Task
                if (found_task) {
                    try {
                        task();
                    } catch (...) {
                        // Task threw
                    }

                    // Simple unbatched atomic update
                    int prev = m_active_tasks.fetch_sub(1, std::memory_order_release);
                    if (prev == 1) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_cv_done.notify_all();
                    }
                }
            }
        }
    };
}
