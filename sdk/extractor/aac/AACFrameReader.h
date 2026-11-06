#pragma once

#include "AudioBuffer.h"
#include "DataSource.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// AAC（ADTS）的包边界扫描器（票面 #62 / ADR 0003）。
//
// ADTS 帧同样自定界：帧长（含 7/9 字节头）写在头里，所以每包一帧、按 offset 现场算，
// init 期只做一次只读扫描拿标称时长（帧数 × 每帧采样数 / 采样率），不留帧表与 payload。
// 起点认不出头时向后找下一个同步点，把这段坏数据当作一包交出去，让解码器失败并记跳帧；
// 找不到就停下——尾部残留是截断，不是坏帧。
class AACFrameReader {
public:
    // 从缓冲里解析一个 ADTS 头：sync + 帧长。available 至少 7 字节。
    static bool parseHeader(const uint8_t *data, size_t available, size_t &frameLength);

    // init 只读扫描：只数帧，不保存任何东西（与旧 countAdtsFrames 的判据完全相同）。
    static size_t scanFrameCount(DataSourceBase *source, int64_t audioStart, int64_t audioEnd);

    AACFrameReader(DataSourceBase *source, int64_t audioStart, int64_t audioEnd);
    ~AACFrameReader();

    void seekTo(int64_t offset);
    int64_t position() const { return m_nextOffset; }
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet);
    size_t heldBytes() const { return m_buf.capacity(); }

private:
    static const size_t kMaxScanBytes = 64 * 1024;
    static const size_t kNotFound     = static_cast<size_t>(-1);
    static const size_t kHeaderBytes  = 7;

    bool emit(size_t length, AudioBuffer::AudioBufferPtr &packet);
    bool appendChunk(size_t want);
    void ensureBytes(size_t want);
    bool atEnd() const;
    size_t findHeader(size_t from);

    DataSourceBase *m_source;
    int64_t m_audioStart;
    int64_t m_audioEnd;
    int64_t m_nextOffset;
    std::vector<uint8_t> m_buf;
};
