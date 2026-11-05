#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// PCM 用的无锁 SPSC ring（规格 #55 第 4.4 节线程约束）：
//   write()  -> 解码线程，唯一生产者
//   read()   -> 消费线程（10 ms push），唯一消费者
//   reset()  -> **仅解码线程**发起，且只在 SEEKING / 切歌的静默窗口里做——此刻消费者
//               正在交付静音、不读 ring。与段基准更新同一窗口完成，先 applySegmentBase()
//               再 reset()，否则位置会先掉回 0 再跳上去。
// ring 本身不加 mutex（SPSC 靠读写指针的 acquire / release 排序），也不持有段基准：
// 段基准存在解码器里，因为 reset() 会把 m_readPos 归零。
// 容量按 2 的幂向上取整。
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
