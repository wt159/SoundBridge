#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"

class WAVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit WAVExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual bool isDataTruncated() const { return m_dataTruncated; }
    // 分配记账：payload 是整块 data chunk（当前链路仍是全量常驻）。
    virtual size_t heldPayloadBytes() const
    {
        return bufferBytes(m_metaBuf) + ExtractorHelper::heldPayloadBytes();
    }
    virtual ~WAVExtractor();

private:
    sdk_utils::status_t init();

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
    AudioBuffer::AudioBufferPtr m_metaBuf;
};
