#pragma once

#include "APEPacketReader.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>

// ape 提取器（票面 #64 / ADR 0003）。
//
// init 期只做一次只读扫描：逐包累加标称时长（包时间轴优先，容器声明只兜底），
// 扫描结束就把 demux 关掉，只留 O(1) 定长摘要：spec、标称时长、codec id、extradata、
// 文件大小、音频包计数。APE 头与帧索引只在扫描时被读一遍，不缓存整首索引、不建 seek 表。
//
// 播放期的包边界由 APEPacketReader 现场重算：APE 没有 ffmpeg parser，边界只能由提取器
// 逐包给出，解码侧按包解码才能覆盖标称时长（整块送进去只解出第一帧）。
class APEExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit APEExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    ~APEExtractor();
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual int getBitRate() { return m_bitRate; }
    virtual int getBlockAlign() { return m_blockAlign; }
    virtual DataSourceBase *getDataSource() { return m_dataSource; }

    // 包工厂的单包面（#64 / ADR 0003）：总数事先未知，边界按 offset 现场重算。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual bool seekPacketByte(int64_t offset);
    virtual int64_t packetBytePosition() const;

    // 分配记账：init 不持有 payload，播放期的 demux 面只持 AVIO 缓冲（判据公式里的
    // 独立项，口径上不混进这个数）；交出去的包归解码侧 alloc/free 记账。
    virtual size_t heldPayloadBytes() const { return 0; }

    // 现场重算包边界的 CPU 成本（独立指标，不设阈值；见规格第 5 节末段）。
    virtual uint64_t packetBoundaryMicros() const
    {
        return m_reader != nullptr ? m_reader->boundaryMicros() : 0;
    }

private:
    sdk_utils::status_t initWithFFmpegDemux();
    // demux 面在**播放期**创建（规格 4.3 所有权清单）。
    APEPacketReader *reader();

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    AudioCodecID m_audioCodecID;
    // 只留 codec 的 extradata（几十字节的定长摘要），不是音频负载。
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    AudioSpec m_audioSpec;
    int m_bitRate;
    int m_blockAlign;
    int64_t m_fileSize;
    // init 只读扫描数到的音频包数：只用来判「有没有音频」，不随播放增长。
    size_t m_audioPacketCount;
    std::unique_ptr<APEPacketReader> m_reader;
    size_t m_nextPacketIndex;
    // 本段包序列的起点（顺序重放 seek 之后为 0）。
    int64_t m_sequenceOffset;
};
