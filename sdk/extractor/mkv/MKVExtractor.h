#pragma once
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "MKVPacketReader.h"
#include "NonCopyable.hpp"
#include <memory>
#include <vector>

// mkv 提取器（票面 #63 / ADR 0003）。
//
// init 期只做一次**只读扫描**：把 demux 从头走到尾，只为按包时间轴累加标称时长
// （容器声明字段不可信：music-mkv.mkv 把 393216 个采样当毫秒写，改口径会动覆盖度基线）。
// 扫描结束就把 demux 关掉，留下的全是 O(1) 定长摘要：spec、标称时长、codec id、
// extradata、文件大小、音频块计数。不保留逐包表、不保留逐包时间轴、不建 seek 表。
//
// 播放期的块边界由 MKVPacketReader 现场重算（demux 在解码线程里创建、随任务释放）。
class MKVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit MKVExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    ~MKVExtractor();
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual int getBitRate() { return m_bitRate; }
    virtual int getBlockAlign() { return m_blockAlign; }

    // —— 包工厂的单包面（#63 / ADR 0003）——
    // 总数事先未知（kUnknownPacketCount = 读到为止）：边界按 offset 现场重算，
    // 任何「先把整份文件数一遍再建表」的做法都会把常驻内存按包数放大。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual bool seekPacketByte(off64_t offset);
    virtual off64_t packetBytePosition() const;

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
    // demux 面在**播放期**创建（规格 4.3 所有权清单：demux 句柄由解码侧创建、随任务释放）。
    MKVPacketReader *reader();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    // 只留 codec 的 extradata（几字节到几十字节的定长摘要），不是音频负载。
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    sdk_utils::status_t m_initCheck;
    AudioSpec m_audioSpec;
    int m_bitRate;
    int m_blockAlign;
    off64_t m_fileSize;
    // init 只读扫描数到的音频块数：只用来判「有没有音频」，不随播放增长。
    size_t m_audioPacketCount;
    std::unique_ptr<MKVPacketReader> m_reader;
    size_t m_nextPacketIndex;
    // 本段包序列的起点（顺序重放 seek 之后为 0：mkv 只有这一档）。
    off64_t m_sequenceOffset;
};
