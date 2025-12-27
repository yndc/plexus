#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new> // hardware_destructive_interference_size
#include <optional>
#include <type_traits>
#include <vector>

namespace Plexus {

    template <typename T> class WorkStealingQueue {
    public:
        explicit WorkStealingQueue(std::size_t capacity = 4096)
            : m_capacity(bit_ceil_at_least_2(capacity)), m_mask(m_capacity - 1),
              m_buffer(m_capacity) {
            static_assert(std::is_move_constructible_v<T>,
                          "WorkStealingQueue requires T to be move-constructible.");

            m_top.store(0, std::memory_order_relaxed);
            m_bottom.store(0, std::memory_order_relaxed);
        }

        WorkStealingQueue(const WorkStealingQueue &) = delete;
        WorkStealingQueue &operator=(const WorkStealingQueue &) = delete;

        std::size_t capacity() const noexcept { return m_capacity; }

        // Owner thread only
        bool push(T value) {
            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_acquire);

            if (b - t >= static_cast<int64_t>(m_capacity)) {
                return false; // full
            }

            m_buffer[static_cast<std::size_t>(b) & m_mask].emplace(std::move(value));

            // Publish: the buffer write must happen-before thieves observe the new bottom.
            m_bottom.store(b + 1, std::memory_order_release);
            return true;
        }

        // Owner thread only (LIFO)
        std::optional<T> pop() {
            int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
            m_bottom.store(b, std::memory_order_relaxed);

            // Chase–Lev requires a global order point between pop/steal.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            int64_t t = m_top.load(std::memory_order_relaxed);

            if (t > b) {
                // Empty: restore bottom
                m_bottom.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }

            const std::size_t idx = static_cast<std::size_t>(b) & m_mask;

            if (t == b) {
                // Last element: must win before touching the slot (no speculative move!)
                if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed)) {
                    // Thief won
                    m_bottom.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }

                // We won: make deque empty (top == bottom)
                m_bottom.store(b + 1, std::memory_order_relaxed);
            }
            // If t < b, it wasn't the last element; no one else touches idx.

            std::optional<T> out;
            out.emplace(std::move(*m_buffer[idx]));
            m_buffer[idx].reset();
            return out;
        }

        // Thieves (FIFO)
        std::optional<T> steal() {
            int64_t t = m_top.load(std::memory_order_acquire);

            // Global order point between steal and owner's pop.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            int64_t b = m_bottom.load(std::memory_order_acquire);

            if (t >= b) {
                return std::nullopt; // empty
            }

            // Claim index t. Only the winner may touch the slot.
            if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                               std::memory_order_relaxed)) {
                return std::nullopt;
            }

            const std::size_t idx = static_cast<std::size_t>(t) & m_mask;

            // bottom.load(acquire) above synchronizes with push's bottom.store(release),
            // so the buffer slot is visible here.
            std::optional<T> out;
            out.emplace(std::move(*m_buffer[idx]));
            m_buffer[idx].reset();
            return out;
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

        std::vector<std::optional<T>> m_buffer;
        std::size_t m_capacity{0};
        std::size_t m_mask{0};
    };

} // namespace Plexus
