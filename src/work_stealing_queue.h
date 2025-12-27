#pragma once

#include <atomic>
#include <cassert>
#include <optional>
#include <vector>

namespace Plexus {

    /**
     * @brief Lock-free work-stealing deque based on the Chase-Lev algorithm.
     *
     * @note CURRENTLY UNUSED: ThreadPool uses RingBuffer with mutexes instead.
     *       This implementation is preserved for potential future use.
     *       See thread_pool.h for the current implementation.
     *
     * This is a single-producer, multi-consumer deque where:
     * - The owner thread can push/pop from one end (LIFO for pop)
     * - Thief threads can steal from the other end (FIFO)
     * - Lock-free for better performance under contention
     *
     * Based on: "Dynamic Circular Work-Stealing Deque" by Chase and Lev (2005)
     */
    template <typename T> class WorkStealingQueue {
    public:
        explicit WorkStealingQueue(size_t capacity = 4096) {
            // Capacity must be power of 2 for fast modulo
            m_capacity = 1;
            while (m_capacity < capacity)
                m_capacity *= 2;
            m_mask = m_capacity - 1;
            m_buffer.resize(m_capacity);
            m_top.store(0, std::memory_order_relaxed);
            m_bottom.store(0, std::memory_order_relaxed);
        }

        // Only called by owner thread
        bool push(T val) {
            int64_t b = m_bottom.load(std::memory_order_relaxed);
            int64_t t = m_top.load(std::memory_order_acquire);

            if (b - t >= static_cast<int64_t>(m_capacity)) {
                // Full
                return false;
            }

            m_buffer[b & m_mask] = std::move(val);
            std::atomic_thread_fence(std::memory_order_release);
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return true;
        }

        // Only called by owner thread (LIFO)
        std::optional<T> pop() {
            int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
            m_bottom.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t t = m_top.load(std::memory_order_relaxed);

            std::optional<T> ret;

            if (t <= b) {
                // Non-empty
                ret = std::move(m_buffer[b & m_mask]);
                if (t == b) {
                    // Last element, race with steal
                    if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                       std::memory_order_relaxed)) {
                        // Lost race to thief
                        ret.reset();
                    }
                    m_bottom.store(b + 1, std::memory_order_relaxed);
                }
            } else {
                // Empty
                m_bottom.store(b + 1, std::memory_order_relaxed);
            }
            return ret;
        }

        // Called by thieves (FIFO)
        std::optional<T> steal() {
            int64_t t = m_top.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t b = m_bottom.load(std::memory_order_acquire);

            if (t < b) {
                // Non-empty - try to claim this slot
                if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed)) {
                    return std::nullopt; // Failed to claim - another thief got it
                }

                // Successfully claimed slot t - now safe to read the buffer
                // No other thread will access this slot now
                return std::move(m_buffer[t & m_mask]);
            }
            return std::nullopt;
        }

        // For testing/sizing
        size_t size() const {
            int64_t b = m_bottom.load(std::memory_order_relaxed);
            int64_t t = m_top.load(std::memory_order_relaxed);
            return (b >= t) ? static_cast<size_t>(b - t) : 0;
        }

        bool empty() const { return size() == 0; }

        // Resizing support needs this
        void resize(size_t new_cap) {
            // Not thread safe, use when quiescent or locked
            if (new_cap < m_capacity)
                return; // Only grow
            // ... strict resizing requires copying tasks which might be moved-from ...
            // Lets assume fixed size for now or full reset.
        }

    private:
        std::atomic<int64_t> m_top;
        std::atomic<int64_t> m_bottom;
        std::vector<T> m_buffer;
        size_t m_capacity;
        size_t m_mask;
    };

} // namespace Plexus
