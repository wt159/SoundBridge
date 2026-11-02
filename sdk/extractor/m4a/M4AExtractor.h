#pragma once
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>

class M4AExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit M4AExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    ~M4AExtractor();
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
    // payload 只保留解复用边界（#17）：AAC 有 parser，拼不拼都能解；ALAC 没有 parser，
    // 一旦拼接就只能解出首帧。统一按包保存，两种 codec 都不会踩坑。
    std::vector<AudioBuffer::AudioBufferPtr> m_packetizedMetaBuf;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    int m_bitRate;
    int m_blockAlign;
};
