#include "AudioRingBuffer.h"
#include <cassert>
#include <cstring>

static size_t nextPowerOfTwo(size_t v)
{
    if (v == 0)
        return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

AudioRingBuffer::AudioRingBuffer(size_t capacity)
    : m_capacity(nextPowerOfTwo(capacity))
    , m_mask(m_capacity - 1)
    , m_buf(new char[m_capacity])
    , m_writePos(0)
    , m_readPos(0)
{
    assert((m_capacity & m_mask) == 0 && "capacity must be power of two");
    memset(m_buf.get(), 0, m_capacity);
}

AudioRingBuffer::~AudioRingBuffer() { }

size_t AudioRingBuffer::availableWrite() const
{
    // Producer reads m_readPos to know how much space is free.
    uint64_t r = m_readPos.load(std::memory_order_acquire);
    uint64_t w = m_writePos.load(std::memory_order_relaxed);
    return m_capacity - static_cast<size_t>(w - r);
}

size_t AudioRingBuffer::write(const char *data, size_t len)
{
    uint64_t w     = m_writePos.load(std::memory_order_relaxed);
    uint64_t r     = m_readPos.load(std::memory_order_acquire);
    size_t space   = m_capacity - static_cast<size_t>(w - r);
    size_t toWrite = (len < space) ? len : space;
    if (toWrite == 0)
        return 0;

    size_t offset = static_cast<size_t>(w) & m_mask;
    size_t tail   = m_capacity - offset;
    if (toWrite <= tail) {
        memcpy(m_buf.get() + offset, data, toWrite);
    } else {
        memcpy(m_buf.get() + offset, data, tail);
        memcpy(m_buf.get(), data + tail, toWrite - tail);
    }
    // Release: makes written data visible to consumer before updating writePos.
    m_writePos.store(w + toWrite, std::memory_order_release);
    return toWrite;
}

size_t AudioRingBuffer::availableRead() const
{
    // Consumer reads m_writePos to know how much data is available.
    uint64_t w = m_writePos.load(std::memory_order_acquire);
    uint64_t r = m_readPos.load(std::memory_order_relaxed);
    return static_cast<size_t>(w - r);
}

size_t AudioRingBuffer::read(char *data, size_t len)
{
    uint64_t r    = m_readPos.load(std::memory_order_relaxed);
    uint64_t w    = m_writePos.load(std::memory_order_acquire);
    size_t avail  = static_cast<size_t>(w - r);
    size_t toRead = (len < avail) ? len : avail;
    if (toRead == 0)
        return 0;

    size_t offset = static_cast<size_t>(r) & m_mask;
    size_t tail   = m_capacity - offset;
    if (toRead <= tail) {
        memcpy(data, m_buf.get() + offset, toRead);
    } else {
        memcpy(data, m_buf.get() + offset, tail);
        memcpy(data + tail, m_buf.get(), toRead - tail);
    }
    // Release: makes read progress visible to producer before updating readPos.
    m_readPos.store(r + toRead, std::memory_order_release);
    return toRead;
}

void AudioRingBuffer::reset()
{
    m_writePos.store(0, std::memory_order_relaxed);
    m_readPos.store(0, std::memory_order_relaxed);
    memset(m_buf.get(), 0, m_capacity);
}
