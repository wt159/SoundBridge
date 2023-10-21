#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"

class WAVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit WAVExtractor(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual ~WAVExtractor();

private:
    sdk_utils::status_t init();

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    uint16_t m_waveFormat;
    uint32_t m_channelMask;
    off64_t m_dataOffset;
    size_t m_dataSize;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
};