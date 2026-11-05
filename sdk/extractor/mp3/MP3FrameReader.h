#pragma once

#include "AudioBuffer.h"
#include "DataSource.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// MPEG 音频帧头解析（原 MP3Extractor.cpp 里的 GetMPEGAudioFrameSize）。
// 抽出来是为了让 init 期的 Resync 与播放期的包工厂共用同一份码表，
// 两处对「什么算合法帧头」不会有第二套解释。
namespace mp3 {

// 同一码流里 version / layer / sampleRate 必须一致；与旧 Resync 的比对掩码相同。
const uint32_t kFrameHeaderMask = 0xfffe0c00;

struct FrameInfo {
    size_t frameSize    = 0;
    int sampleRate      = 0;
    int channels        = 0;
    int bitrateKbps     = 0;
    int samplesPerFrame = 0;
    bool valid          = false;
};

FrameInfo parseFrameHeader(uint32_t header);

} // namespace mp3

// MP3 的包边界扫描器（ADR 0003：包为层间唯一单元、init 只留 O(1) 摘要）。
//
// MPEG 帧是自定界的：帧长由帧头里的码率 / 采样率 / padding / layer 直接算出，
// 不需要预扫描或建索引。包 = 一个完整 MPEG 帧，与 ffmpeg 的 parser 切出来的边界一致，
// 但解码侧因此可以逐包直喂编解码器（不经过 parser），坏数据段才会作为独立包被跳过。
//
// 起点认不出帧头时向后找下一个通过掩码检查的帧头，把这段坏数据当作一包交出去，
// 让解码器自己去失败并记一次跳帧（规格 4.1：坏帧之后还有数据）。找不到就停下，
// 尾部残留不交出去——截断不是坏帧。可解性不由这里判断（ADR 0002：拒绝的唯一出口
// 仍是 createExtractor() == nullptr）。
class MP3FrameReader {
public:
    MP3FrameReader(DataSourceBase *source, off64_t audioStart, off64_t audioEnd);
    ~MP3FrameReader();

    // 定位到任意文件偏移：下一包从该处重新认帧头。offset 会被夹到 [audioStart, audioEnd]。
    void seekTo(off64_t offset);
    off64_t position() const { return m_nextOffset; }

    bool nextPacket(AudioBuffer::AudioBufferPtr &packet);

    // 已分配且尚未释放的字节：只有扫描缓冲的容量（交出后的包归解码侧记账）。
    size_t heldBytes() const { return m_buf.capacity(); }

private:
    // 坏数据段的扫描上限：坏帧可能没有下一个同步点，不能为了「找到下一帧」把整首读进来。
    static const size_t kMaxScanBytes = 64 * 1024;
    static const size_t kNotFound     = static_cast<size_t>(-1);

    bool emit(size_t length, AudioBuffer::AudioBufferPtr &packet);
    bool appendChunk(size_t want);
    void ensureBytes(size_t want);
    bool atEnd() const;
    // pos 处的 4 字节是否是候选帧头；能读到后继帧头时额外要求掩码一致。
    bool candidateAt(size_t pos) const;
    size_t findHeader(size_t from);

    DataSourceBase *m_source;
    off64_t m_audioStart;
    off64_t m_audioEnd;
    off64_t m_nextOffset;
    std::vector<uint8_t> m_buf;
};
