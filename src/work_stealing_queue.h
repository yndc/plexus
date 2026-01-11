#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new> // hardware_destructive_interference_size
#include <vector>

namespace Plexus {

    /**
     * Lock-free work-stealing deque based on the Chase-Lev algorithm.
     *
     * Implementation follows "Correct and Efficient Work-Stealing for Weak
     * Memory Models" (Lê et al., PPoPP 2013) for memory ordering.
     *
     * - Owner thread: push() and pop() from the "bottom" (LIFO)
     * - Thief threads: steal() from the "top" (FIFO)
     *
     * This queue stores raw pointers (T*) with stable lifetimes.
     * The caller is responsible for managing the pointed-to objects
     * (typically via a pool). The queue never allocates or frees memory.
     */
    template <typename T> class WorkStealingQueue {
    public:
        explicit WorkStealingQueue(std::size_t capacity = 4096)
            : m_capacity(bit_ceil_at_least_2(capacity)), m_mask(m_capacity - 1),
              m_buffer(m_capacity) {

            m_top.store(0, std::memory_order_relaxed);
            m_bottom.store(0, std::memory_order_relaxed);

            for (auto &slot : m_buffer) {
                slot.store(nullptr, std::memory_order_relaxed);
            }
        }

        // Destructor does NOT free pointers - caller owns them via pool
        ~WorkStealingQueue() = default;

        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        std::size_t capacity() const noexcept { return m_capacity; }

        /**
         * @brief Push a pointer to the bottom of the queue (owner only).
         * @return true if pushed, false if queue is full.
         */
        bool push(T *ptr) {
            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_acquire);

            if (b - t >= static_cast<int64_t>(m_capacity)) {
                return false; // full
            }

            const std::size_t idx = static_cast<std::size_t>(b) & m_mask;

            // Store pointer. The release on bottom below will make this visible.
            m_buffer[idx].store(ptr, std::memory_order_relaxed);

            // Publish: this release pairs with acquire in steal's buffer load.
            m_bottom.store(b + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Pop a pointer from the bottom of the queue (owner only, LIFO).
         * @return Pointer if available, nullptr if queue is empty or lost race.
         */
        T *pop() {
            int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;

            // seq_cst store to synchronize with steal's operations
            m_bottom.store(b, std::memory_order_seq_cst);

            int64_t t = m_top.load(std::memory_order_seq_cst);

            if (t > b) {
                // Queue was empty, restore bottom
                m_bottom.store(b + 1, std::memory_order_relaxed);
                return nullptr;
            }

            const std::size_t idx = static_cast<std::size_t>(b) & m_mask;

            // Read pointer BEFORE potential CAS (critical for t == b case).
            // Chase-Lev requires reading element before CAS because after
            // CAS the slot may be refilled by a concurrent push.
            T *ptr = m_buffer[idx].load(std::memory_order_acquire);

            if (t == b) {
                // Last element - race with thieves for the same slot.
                if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed)) {
                    // A thief won - abort without touching ptr
                    m_bottom.store(b + 1, std::memory_order_relaxed);
                    return nullptr;
                }
                // We won - restore bottom (queue now empty)
                m_bottom.store(b + 1, std::memory_order_relaxed);
            }
            // If t < b: exclusive access to this slot, no CAS needed.

            // DO NOT clear the slot - it may be reused after wraparound.
            // The pointer has stable lifetime (managed by pool).

            assert(ptr != nullptr && "pop() won but ptr is null - algorithm bug");
            return ptr;
        }

        /**
         * @brief Steal a pointer from the top of the queue (thief threads, FIFO).
         * @return Pointer if available, nullptr if queue is empty or lost race.
         */
        T *steal() {
            int64_t t = m_top.load(std::memory_order_seq_cst);
            int64_t b = m_bottom.load(std::memory_order_seq_cst);

            if (t >= b) {
                return nullptr; // empty
            }

            const std::size_t idx = static_cast<std::size_t>(t) & m_mask;

            // Read pointer BEFORE CAS (critical!).
            // Chase-Lev requires reading element before CAS because after
            // CAS the slot may be refilled by a concurrent push.
            T *ptr = m_buffer[idx].load(std::memory_order_acquire);

            // Attempt to claim this slot by incrementing top
            if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                               std::memory_order_relaxed)) {
                // Lost the race - abort, do NOT use ptr
                return nullptr;
            }

            // We won the CAS. DO NOT clear the slot - it may be reused after wraparound.
            // The pointer has stable lifetime (managed by pool).

            assert(ptr != nullptr && "steal() won but ptr is null - algorithm bug");
            return ptr;
        }

        // Approximate under concurrency; exact only when quiescent.
        std::size_t size_approx() const noexcept {
            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_relaxed);
            return (b >= t) ? static_cast<std::size_t>(b - t) : 0u;
        }

        bool empty_approx() const noexcept { return size_approx() == 0u; }

    private:
        static constexpr std::size_t cacheline_size() noexcept {
#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size >= 201703L
            return std::hardware_destructive_interference_size;
#else
            return 64; // reasonable fallback
#endif
        }

        static std::size_t bit_ceil_at_least_2(std::size_t x) {
            x = (x < 2) ? 2 : x;
#if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L
            return std::bit_ceil(x);
#else
            std::size_t cap = 2;
            while (cap < x)
                cap <<= 1;
            return cap;
#endif
        }

        alignas(cacheline_size()) std::atomic<int64_t> m_top{0};
        alignas(cacheline_size()) std::atomic<int64_t> m_bottom{0};

        std::size_t m_capacity{0};
        std::size_t m_mask{0};
        std::vector<std::atomic<T *>> m_buffer;
    };

} // namespace Plexus
