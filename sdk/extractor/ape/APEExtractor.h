#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>
#include <string>

class APEExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit APEExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual int getBitRate() { return m_bitRate; }
    virtual int getBlockAlign() { return m_blockAlign; }
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
    AudioBuffer::AudioBufferPtr m_metaBuf;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    int m_bitRate;
    int m_blockAlign;
};
