#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new> // hardware_destructive_interference_size
#include <optional>
#include <type_traits>
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
     * Uses atomic<T*> for buffer elements to ensure thread-safe access.
     */
    template <typename T> class WorkStealingQueue {
    public:
        explicit WorkStealingQueue(std::size_t capacity = 4096)
            : m_capacity(bit_ceil_at_least_2(capacity)), m_mask(m_capacity - 1),
              m_buffer(m_capacity) {
            static_assert(std::is_move_constructible_v<T>,
                          "WorkStealingQueue requires T to be move-constructible.");

            m_top.store(0, std::memory_order_relaxed);
            m_bottom.store(0, std::memory_order_relaxed);

            for (auto &slot : m_buffer) {
                slot.store(nullptr, std::memory_order_relaxed);
            }
        }

        ~WorkStealingQueue() {
            // Clean up any remaining elements
            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_relaxed);
            for (int64_t i = t; i < b; ++i) {
                const std::size_t idx = static_cast<std::size_t>(i) & m_mask;
                delete m_buffer[idx].load(std::memory_order_relaxed);
            }
        }

        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        std::size_t capacity() const noexcept { return m_capacity; }

        // Owner thread only
        template <typename U = T> bool push(U &&value) {
            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_acquire);

            if (b - t >= static_cast<int64_t>(m_capacity)) {
                return false; // full
            }

            const std::size_t idx = static_cast<std::size_t>(b) & m_mask;

            // Slot should be empty (cleared by pop/steal). Debug assertion:
            // assert(m_buffer[idx].load(std::memory_order_relaxed) == nullptr);

            T *ptr = new T(std::forward<U>(value));

            // Store pointer. The release on bottom below will make this visible.
            m_buffer[idx].store(ptr, std::memory_order_relaxed);

            // Publish: this release pairs with acquire in steal's buffer load.
            m_bottom.store(b + 1, std::memory_order_release);
            return true;
        }

        // Owner thread only (LIFO)
        std::optional<T> pop() {
            int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;

            // seq_cst store to synchronize with steal's operations
            m_bottom.store(b, std::memory_order_seq_cst);

            int64_t t = m_top.load(std::memory_order_seq_cst);

            if (t > b) {
                // Queue was empty, restore bottom
                m_bottom.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
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
                    return std::nullopt;
                }
                // We won - restore bottom (queue now empty)
                m_bottom.store(b + 1, std::memory_order_relaxed);
            }
            // If t < b: exclusive access to this slot, no CAS needed.

            // DO NOT clear the slot - it may be reused after wraparound.
            // Just consume the pointer we already read.

            // Debug assertion: ptr should never be null after a successful claim
            assert(ptr != nullptr && "pop() won CAS but ptr is null - algorithm bug");

            std::optional<T> result;
            result.emplace(std::move(*ptr));
            delete ptr;
            return result;
        }

        // Thief threads (FIFO)
        std::optional<T> steal() {
            int64_t t = m_top.load(std::memory_order_seq_cst);
            int64_t b = m_bottom.load(std::memory_order_seq_cst);

            if (t >= b) {
                return std::nullopt; // empty
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
                return std::nullopt;
            }

            // We won the CAS. DO NOT clear the slot - it may be reused after wraparound.
            // Just consume the pointer we already read.

            // Debug assertion: ptr should never be null after a successful claim
            assert(ptr != nullptr && "steal() won CAS but ptr is null - algorithm bug");

            std::optional<T> result;
            result.emplace(std::move(*ptr));
            delete ptr;
            return result;
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
