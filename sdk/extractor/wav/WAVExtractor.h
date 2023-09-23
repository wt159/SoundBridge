#pragma once

#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"

class WAVExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit WAVExtractor(DataSourceHelper *source);

    virtual size_t countTracks();

    // virtual MediaTrackHelper *getTrack(size_t index);

    virtual ~WAVExtractor();

private:
    status_t init();

private:
    DataSourceHelper *m_dataSource;
    status_t m_initCheck;
    bool m_validFormat;
    uint16_t m_waveFormat;
    uint16_t m_numChannels;
    uint32_t m_channelMask;
    uint32_t m_sampleRate;
    uint16_t m_bitsPerSample;
    int64_t m_durationUs;
    off64_t m_dataOffset;
    size_t m_dataSize;
};