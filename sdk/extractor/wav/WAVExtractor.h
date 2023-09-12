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
    DataSourceHelper *mDataSource;
    status_t mInitCheck;
    bool mValidFormat;
    uint16_t mWaveFormat;
    uint16_t mNumChannels;
    uint32_t mChannelMask;
    uint32_t mSampleRate;
    uint16_t mBitsPerSample;
    int64_t mDurationUs;
    off64_t mDataOffset;
    size_t mDataSize;
};