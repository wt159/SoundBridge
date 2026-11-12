#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include "PcmPacketReader.hpp"
#include <memory>

class WAVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit WAVExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual bool isDataTruncated() const { return m_dataTruncated; }
    DataSourceBase *getDataSource() override { return m_dataSource; }

    // —— 包工厂的单包面（#61 / ADR 0003）——
    // init 期只留 O(1) 定长摘要（spec / 时长 / data 块的偏移与长度 / 截断标志），
    // 负载在播放期按 offset 现场切片读出，不再整首常驻。总数事先未知，
    // 用 kUnknownPacketCount 表示「读到为止」；一包 = 一段对齐到帧的定长 PCM 切片。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    // 下一包的文件偏移（数据块内按整数帧推进）。
    virtual int64_t packetBytePosition() const;

    // 分配记账：init 不持 payload，交出去的包归解码侧记账（alloc/free 增量）。
    virtual size_t heldPayloadBytes() const { return 0; }
    virtual ~WAVExtractor();

private:
    sdk_utils::status_t init();
    // 切片面在**播放期**创建（规格 4.3：demux 由解码侧持有、随任务释放）。
    PcmPacketReader *reader();

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    // 声明的 data 长度 > 文件实际剩余：短读在字节层面可判定（规格 T2）。
    bool m_dataTruncated;
    uint16_t m_waveFormat;
    uint32_t m_channelMask;
    off64_t m_dataOffset;
    size_t m_dataSize;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    std::unique_ptr<PcmPacketReader> m_reader;
    size_t m_nextPacketIndex;
};
