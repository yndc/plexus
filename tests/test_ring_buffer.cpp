#include "../src/ring_buffer.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace Plexus;

TEST(RingBufferTest, BasicPushPop) {
    RingBuffer<int> buf;
    buf.push(1);
    buf.push(2);
    buf.push(3);

    EXPECT_EQ(buf.size(), 3u);
    EXPECT_FALSE(buf.empty());

    int out;
    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, 1); // FIFO

    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, 2);

    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, 3);

    EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, PopBackLIFO) {
    RingBuffer<int> buf;
    buf.push(1);
    buf.push(2);
    buf.push(3);

    int out;
    EXPECT_TRUE(buf.pop_back(out));
    EXPECT_EQ(out, 3); // LIFO

    EXPECT_TRUE(buf.pop_back(out));
    EXPECT_EQ(out, 2);

    EXPECT_TRUE(buf.pop_back(out));
    EXPECT_EQ(out, 1);

    EXPECT_TRUE(buf.empty());
}

TEST(RingBufferTest, PopBatch) {
    RingBuffer<int> buf;
    for (int i = 0; i < 10; ++i) {
        buf.push(i);
    }

    std::vector<int> batch;
    size_t popped = buf.pop_batch(batch, 5);
    EXPECT_EQ(popped, 5u);
    EXPECT_EQ(batch.size(), 5u);
    EXPECT_EQ(buf.size(), 5u);

    // Verify FIFO order
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(batch[i], i);
    }
}

TEST(RingBufferTest, EmptyPopReturnsFalse) {
    RingBuffer<int> buf;
    int out;
    EXPECT_FALSE(buf.pop(out));
    EXPECT_FALSE(buf.pop_back(out));
}

TEST(RingBufferTest, EmptyBatchReturnsZero) {
    RingBuffer<int> buf;
    std::vector<int> batch;
    EXPECT_EQ(buf.pop_batch(batch, 10), 0u);
    EXPECT_TRUE(batch.empty());
}

TEST(RingBufferTest, AutoResize) {
    RingBuffer<int> buf;
    EXPECT_EQ(buf.capacity(), 0u);

    // Push triggers initial resize to 16
    buf.push(1);
    EXPECT_GE(buf.capacity(), 16u);

    // Push many more to trigger resize
    for (int i = 0; i < 100; ++i) {
        buf.push(i);
    }

    EXPECT_GE(buf.capacity(), 101u);
    EXPECT_EQ(buf.size(), 101u);

    // Verify data integrity after resize
    int out;
    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, 1); // First pushed
}

TEST(RingBufferTest, WrapAround) {
    RingBuffer<int> buf;
    buf.resize(4);

    // Fill buffer
    buf.push(1);
    buf.push(2);
    buf.push(3);
    buf.push(4);

    // Pop half
    int out;
    buf.pop(out);
    buf.pop(out);

    // Push more (wraps around)
    buf.push(5);
    buf.push(6);

    // Should get 3, 4, 5, 6 in order
    std::vector<int> result;
    while (buf.pop(out)) {
        result.push_back(out);
    }

    EXPECT_EQ(result, (std::vector<int>{3, 4, 5, 6}));
}

TEST(RingBufferTest, MoveOnlyType) {
    RingBuffer<std::unique_ptr<int>> buf;
    buf.push(std::make_unique<int>(42));
    buf.push(std::make_unique<int>(100));

    std::unique_ptr<int> out;
    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(*out, 42);

    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(*out, 100);
}

TEST(RingBufferTest, FullQueryAfterResize) {
    RingBuffer<int> buf;
    buf.resize(4);

    EXPECT_FALSE(buf.full());
    buf.push(1);
    buf.push(2);
    buf.push(3);
    buf.push(4);
    EXPECT_TRUE(buf.full());

    // Auto-extend on push
    buf.push(5);
    EXPECT_FALSE(buf.full());
    EXPECT_EQ(buf.size(), 5u);
}

TEST(RingBufferTest, SizeAndCapacity) {
    RingBuffer<int> buf;
    buf.resize(32);

    EXPECT_EQ(buf.capacity(), 32u);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.empty());

    for (int i = 0; i < 20; ++i) {
        buf.push(i);
    }

    EXPECT_EQ(buf.size(), 20u);
    EXPECT_EQ(buf.capacity(), 32u);
}

TEST(RingBufferTest, ResizeSmaller) {
    RingBuffer<int> buf;
    buf.resize(32);
    buf.push(1);
    buf.push(2);

    // Resize to smaller should be no-op
    buf.resize(16);
    EXPECT_EQ(buf.capacity(), 32u);
    EXPECT_EQ(buf.size(), 2u);
}

TEST(RingBufferTest, StringType) {
    RingBuffer<std::string> buf;
    buf.push("hello");
    buf.push("world");

    std::string out;
    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, "hello");

    EXPECT_TRUE(buf.pop(out));
    EXPECT_EQ(out, "world");
}
