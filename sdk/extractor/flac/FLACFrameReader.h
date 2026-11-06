#pragma once

#include "AudioBuffer.h"
#include "DataSource.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// FLAC 的包边界扫描器（ADR 0003「包为唯一层间单元 / 读过 ≠ 常驻 / init 只留 O(1) 摘要」）。
//
// FLAC 没有 ffmpeg demux：帧边界只能按 offset 现场重算，做法与 libFLAC 自己的 seek 一致：
//   1) 帧头：14 位同步码 + 保留位与码表检查 + 帧头 CRC-8；
//   2) 找下一个通过了同样检查的位置，再用帧尾 CRC-16 确认整帧自洽——只有 CRC-16 也过
//      才算边界，否则继续往后找（坏数据处不会把一帧切错）。
// 起点认不出帧头、或整段都找不到边界时，把「到下一个同步点 / 到扫描上限」的数据当作一包
// 交出去，让 libFLAC 自己 resync；可解性不由这里判断（ADR 0002：拒绝的唯一出口是
// createExtractor() == nullptr，包耗尽 + ring 0 字节 + EOS 汇总才是播放期的兜底）。
//
// 常驻状态是 O(1)：一个读缓冲（容量封顶）+ 下一包的文件偏移；不存包表、不存时间轴索引，
// 也不缓存已产出的包（交出后由调用方持有）。
class FLACFrameReader {
public:
    // streamHeaderEnd：音频数据起点（fLaC + 元数据块之后）；首包是 [0, streamHeaderEnd)，
    // 因为 libFLAC 要从 STREAMINFO 起读才会打开整段 MD5 校验。
    // maxFrameBytes：STREAMINFO 声明的最大帧字节数，用来定扫描缓冲的上限（0 表示未知）。
    FLACFrameReader(DataSourceBase *source, int64_t streamHeaderEnd, int64_t endOffset,
                    size_t maxFrameBytes);
    ~FLACFrameReader();

    // 定位到任意文件偏移（native seek 用）：下一包从该处重新认帧头。
    void seekTo(int64_t offset);
    int64_t position() const { return m_nextOffset; }

    // 拉下一包：首包是流头，之后每包一帧。返回 false 表示序列正常结束。
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet);

    // 已分配且尚未释放的字节：只有读缓冲的容量（交出后的包归解码侧记账）。
    size_t heldBytes() const { return m_buf.capacity(); }

private:
    // 扫描缓冲的硬上限：坏数据处也不会为了「找到下一帧」把整首读进内存。
    static const size_t kMaxScanBytes = 1024 * 1024;

    bool emit(size_t length, AudioBuffer::AudioBufferPtr &packet);
    bool produceFrame(AudioBuffer::AudioBufferPtr &packet);
    // 把 [m_nextOffset + m_buf.size(), +want) 读进缓冲；返回是否读到了数据。
    bool appendChunk(size_t want);
    void ensureBytes(size_t want);
    bool atEnd() const;
    bool headerAt(size_t offset, size_t &headerBytes) const;
    // 在缓冲里找下一个候选帧头（下标）；kNotFound 表示当前缓冲内没有。
    static const size_t kNotFound = static_cast<size_t>(-1);
    size_t findHeader(size_t from, size_t &scanned);

    DataSourceBase *m_source;
    int64_t m_streamHeaderEnd;
    int64_t m_endOffset;
    int64_t m_nextOffset;
    size_t m_scanCap;
    std::vector<uint8_t> m_buf;
};
