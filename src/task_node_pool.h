#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>

namespace Plexus {

    /**
     * @brief Fixed-size, type-erased callable with small-buffer optimization.
     *
     * Avoids heap allocation for small lambdas (up to kStorageSize bytes).
     */
    template <std::size_t StorageSize = 64> class FixedFunction {
    public:
        FixedFunction() = default;

        template <typename F> FixedFunction(F &&f) {
            static_assert(sizeof(F) <= StorageSize, "Task too large for FixedFunction");
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
                other.m_invoke = nullptr;
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

        void reset() {
            if (m_invoke) {
                m_dtor(m_storage);
                m_invoke = nullptr;
            }
        }

    private:
        alignas(std::max_align_t) std::byte m_storage[StorageSize]{};
        void (*m_invoke)(void *) = nullptr;
        void (*m_dtor)(void *) = nullptr;
        void (*m_move)(void *dest, void *src) = nullptr;
    };

    /**
     * @brief A node for the task pool, containing a task and freelist pointer.
     *
     * TaskNodes have stable addresses and are never freed during execution,
     * only returned to the pool. This eliminates use-after-free in the
     * work-stealing queue.
     */
    struct TaskNode {
        std::atomic<TaskNode *> next{nullptr};
        FixedFunction<64> task;

        void reset() {
            next.store(nullptr, std::memory_order_relaxed);
            task.reset();
        }
    };

    /**
     * @brief Lock-free pool for TaskNode allocation using Treiber stack.
     *
     * Provides O(1) allocation and deallocation. Nodes are never freed
     * until pool destruction, ensuring stable lifetimes for the work-stealing queue.
     */
    class TaskNodePool {
    public:
        TaskNodePool() = default;

        ~TaskNodePool() {
            // Free all nodes in the freelist
            TaskNode *node = m_head.load(std::memory_order_relaxed);
            while (node) {
                TaskNode *next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }

            // Note: nodes currently in queues are NOT freed here.
            // The ThreadPool must ensure all workers are stopped before destruction.
        }

        TaskNodePool(const TaskNodePool &) = delete;
        TaskNodePool &operator=(const TaskNodePool &) = delete;

        /**
         * @brief Allocate a TaskNode from the pool.
         *
         * Pops from freelist if available, otherwise allocates new.
         */
        TaskNode *alloc() {
            // Try to pop from freelist (lock-free Treiber pop)
            TaskNode *head = m_head.load(std::memory_order_acquire);
            while (head) {
                TaskNode *next = head->next.load(std::memory_order_relaxed);
                if (m_head.compare_exchange_weak(head, next, std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    head->reset();
                    return head;
                }
                // head updated by CAS, retry
            }

            // Freelist empty, allocate new
            m_allocated.fetch_add(1, std::memory_order_relaxed);
            return new TaskNode();
        }

        /**
         * @brief Return a TaskNode to the pool.
         *
         * Pushes to freelist (lock-free Treiber push).
         */
        void free(TaskNode *node) {
            if (!node)
                return;

            // Lock-free Treiber push
            TaskNode *head = m_head.load(std::memory_order_relaxed);
            do {
                node->next.store(head, std::memory_order_relaxed);
            } while (!m_head.compare_exchange_weak(head, node, std::memory_order_release,
                                                   std::memory_order_relaxed));
        }

        /**
         * @brief Returns approximate number of nodes ever allocated.
         */
        std::size_t allocated_count() const { return m_allocated.load(std::memory_order_relaxed); }

    private:
        std::atomic<TaskNode *> m_head{nullptr};
        std::atomic<std::size_t> m_allocated{0};
    };

} // namespace Plexus
