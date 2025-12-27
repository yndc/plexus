#pragma once
#include <memory>
#include <vector>

namespace Plexus {

    /**
     * @brief A fixed-size circular buffer.
     * Not thread-safe. Must be protected by external synchronization.
     */
    template <typename T> class RingBuffer {
    public:
        RingBuffer() = default;

        /**
         * @brief Resizes the buffer to the specified capacity.
         * WARNING: This invalidates existing contents. safely use only when empty.
         */
        void resize(size_t new_capacity) {
            if (new_capacity <= m_capacity)
                return;

            auto new_buffer = std::make_unique<T[]>(new_capacity);
            if (m_count > 0) {
                size_t current = m_head;
                for (size_t i = 0; i < m_count; ++i) {
                    new_buffer[i] = std::move(m_buffer[current]);
                    current = (current + 1) % m_capacity;
                }
            }

            m_buffer = std::move(new_buffer);
            m_capacity = new_capacity;
            m_head = 0;
            m_tail = m_count;
        }

        /**
         * @brief Pushes an item into the buffer.
         * Auto-resizes if full.
         * @return true always (unless allocation fails).
         */
        bool push(T item) {
            if (m_count == m_capacity) {
                resize(m_capacity == 0 ? 16 : m_capacity * 2);
            }

            m_buffer[m_tail] = std::move(item);
            m_tail = (m_tail + 1) % m_capacity;
            m_count++;
            return true;
        }

        /**
         * @brief Pops an item from the buffer.
         * @return true if successful, false if empty.
         */
        bool pop(T &out_item) {
            if (m_count == 0)
                return false;

            out_item = std::move(m_buffer[m_head]);
            m_head = (m_head + 1) % m_capacity;
            m_count--;
            return true;
        }

        bool pop_back(T &out_item) {
            if (m_count == 0)
                return false;

            size_t tail_idx = (m_tail == 0 ? m_capacity : m_tail) - 1;
            out_item = std::move(m_buffer[tail_idx]);
            m_tail = tail_idx;
            m_count--;
            return true;
        }

        size_t pop_batch(std::vector<T> &out_batch, size_t max_count) {
            if (m_count == 0)
                return 0;

            size_t count = std::min(m_count, max_count);
            for (size_t i = 0; i < count; ++i) {
                out_batch.push_back(std::move(m_buffer[m_head]));
                m_head = (m_head + 1) % m_capacity;
            }
            m_count -= count;
            return count;
        }

        [[nodiscard]] bool empty() const { return m_count == 0; }
        [[nodiscard]] bool full() const { return m_count == m_capacity; }
        [[nodiscard]] size_t size() const { return m_count; }
        [[nodiscard]] size_t capacity() const { return m_capacity; }

    private:
        std::unique_ptr<T[]> m_buffer;
        size_t m_capacity = 0;
        size_t m_head = 0;
        size_t m_tail = 0;
        size_t m_count = 0;
    };

} // namespace Plexus
