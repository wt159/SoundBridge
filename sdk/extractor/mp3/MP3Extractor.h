#pragma once
#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

/*
 *  MP3 file format
 *
 *  ID3v2
 *    +
 *  AudioFrame
 *   +
 *  ID3v1
 */

class MP3Extractor : public ExtractorHelper, public NonCopyable {
private:
    status_t m_initCheck;
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    bool m_validFormat;
    AudioSpec m_spec;
    off64_t m_firstFramePos;
    uint32_t m_fixedHeader;

public:
    explicit MP3Extractor(DataSourceBase *source);
    ~MP3Extractor() = default;
    virtual status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }

private:
    status_t init();
};
