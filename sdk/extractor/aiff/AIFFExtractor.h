#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include "PcmPacketReader.hpp"
#include <memory>

namespace AIFF {

enum {
    kChunkID_FORM = 0x464F524D, /* 'FORM' */
    kChunkID_COMM = 0x434F4D4D, /* 'COMM' */
    kChunkID_SSND = 0x53534E44, /* 'SSND' */
    kChunkID_FVER = 0x46564552, /* 'FVER' */
};

struct Chunk {
    uint32_t chunkID;
    uint32_t chunkSize;
};

struct CommonChunk : public Chunk {
    /* ID 'COMM' */
    /* size 30 + name.len() */
    uint16_t numChannels;
    uint32_t numSampleFrames;
    uint16_t sampleSize;
    uint32_t sampleRate; /* 80-bit floating point */
};

}

class AIFFExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit AIFFExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual bool isDataTruncated() const { return m_dataTruncated; }

    // —— 包工厂的单包面（#61 / ADR 0003）——
    // init 期只留 O(1) 定长摘要（spec / 时长 / SSND 的偏移与长度 / 截断标志），
    // 负载在播放期按 offset 现场切片读出。总数事先未知，用 kUnknownPacketCount 表示「读到为止」；
    // 一包 = 一段对齐到帧的定长 PCM 切片，交出之前换成主机字节序（AIFF 的 PCM 是大端）。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual off64_t packetBytePosition() const;

    // 分配记账：init 只留 O(1) 标量摘要，不持 payload；
    // 交出去的包归解码侧记账（alloc/free 增量）。
    virtual size_t heldPayloadBytes() const { return 0; }
    virtual ~AIFFExtractor();

private:
    sdk_utils::status_t init();
    // 切片面在**播放期**创建（规格 4.3：demux 由解码侧持有、随任务释放）。
    PcmPacketReader *reader();
    void commonChunk2AudioSpec(const AIFF::CommonChunk *cChunk, AudioSpec &audioSpec);
    void AudioBuffer2HostEndian(AudioBuffer::AudioBufferPtr &audioBuf, const AudioSpec &spec);

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    // 声明的 SSND 长度 > 文件实际剩余：短读在字节层面可判定（规格 T2）。
    bool m_dataTruncated;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    // SSND 的阴影数据起点与长度（clamp 到文件剩余）：O(1) 摘要。
    off64_t m_dataOffset;
    size_t m_dataSize;
    std::unique_ptr<PcmPacketReader> m_reader;
    size_t m_nextPacketIndex;
};
