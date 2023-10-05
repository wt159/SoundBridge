#pragma once

#include "ExtractorHelper.hpp"
#include "DataSource.hpp"
#include "AudioDecode.h"
#include "NonCopyable.hpp"

class WAVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit WAVExtractor(DataSourceBase *source);

    virtual uint64_t getDurationMs() { return m_durationMs; }
    virtual off64_t getDataSize()  { return m_dataSize; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
     virtual void readAudioRawData(AudioBuffer::AudioBufferPtr &bufPtr);
    virtual ~WAVExtractor();

private:
    status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioDecode *m_decode;
    status_t m_initCheck;
    bool m_validFormat;
    uint16_t m_waveFormat;
    uint16_t m_numChannels;
    uint32_t m_channelMask;
    uint32_t m_sampleRate;
    uint16_t m_bitsPerSample;
    uint64_t m_durationMs;
    off64_t m_dataOffset;
    size_t m_dataSize;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_rawBuf;
};