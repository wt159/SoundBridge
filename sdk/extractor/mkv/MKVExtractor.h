#pragma once
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>
#include <vector>

class MKVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit MKVExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    ~MKVExtractor();
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual std::vector<AudioBuffer::AudioBufferPtr> getPacketizedMetaData() const
    {
        return m_packetizedMetaBuf;
    }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual int getBitRate() { return m_bitRate; }
    virtual int getBlockAlign() { return m_blockAlign; }
    // 分配记账：逐包 payload + 包指针槽位。
    virtual size_t heldPayloadBytes() const
    {
        return packetFootprint(m_packetizedMetaBuf) + ExtractorHelper::heldPayloadBytes();
    }

private:
    sdk_utils::status_t initWithFFmpegDemux();
    struct AvioData {
        DataSourceBase *source;
        int64_t pos;
        int64_t size;
    };
    static int avioRead(void *opaque, uint8_t *buf, int buf_size);
    static int64_t avioSeek(void *opaque, int64_t offset, int whence);

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    // payload 只保留解复用边界（#17）；需要整块的消费者走基类 getMetaData() 按需拼合。
    std::vector<AudioBuffer::AudioBufferPtr> m_packetizedMetaBuf;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    int m_bitRate;
    int m_blockAlign;
};
