#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>
#include <string>
#include <vector>

class APEExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit APEExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
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
    // 分配记账：逐包 payload（APE 没有 parser，包边界是唯一表示）+ 包指针槽位。
    virtual size_t heldPayloadBytes() const
    {
        return packetFootprint(m_packetizedMetaBuf) + ExtractorHelper::heldPayloadBytes();
    }
    virtual ~APEExtractor();

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
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    // 逐包保留原始帧：APE 在 ffmpeg 里没有 parser，AudioDecode 一旦拿到「拼接成一整块」
    // 的输入就只能解出第一帧，必须让解码器按 packet 逐帧解。
    // 只保存这一种表示；需要整块的消费者由 ExtractorHelper::getMetaData() 按需拼合（#17）。
    std::vector<AudioBuffer::AudioBufferPtr> m_packetizedMetaBuf;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    int m_bitRate;
    int m_blockAlign;
};
