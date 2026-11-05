#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// Lock-free Single-Producer Single-Consumer ring buffer for PCM audio.
// write() -> decode thread only.  read() -> audio callback thread only.
// reset() -> only when both threads are quiesced (seek / track switch).
// Capacity is rounded up to the next power of two internally.
class AudioRingBuffer {
public:
    static constexpr size_t kDefaultCapacity = 262144; // 256 KB

    explicit AudioRingBuffer(size_t capacity = kDefaultCapacity);
    ~AudioRingBuffer();

    size_t write(const char *data, size_t len);
    size_t availableWrite() const;

    size_t read(char *data, size_t len);
    size_t availableRead() const;

    // 本段内已被消费端取走的字节数（从 reset() 起算）。
    // 播放位置 = 消费端段基准 + 本值；段基准不在 ring 里，因为 reset() 会把它归零。
    uint64_t readPos() const { return m_readPos.load(std::memory_order_acquire); }

    void reset();

    size_t capacity() const { return m_capacity; }

private:
    const size_t m_capacity;
    const size_t m_mask;
    std::unique_ptr<char[]> m_buf;
    std::atomic<uint64_t> m_writePos;
    std::atomic<uint64_t> m_readPos;
};
