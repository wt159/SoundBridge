#pragma once

#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

struct ADTSHeader {
    uint16_t syncWord              : 12;
    uint8_t id                     : 1;
    uint8_t layer                  : 2;
    uint8_t protectionAbsent       : 1;
    uint8_t profile                : 2;
    uint8_t samplingFrequencyIndex : 4;
    uint8_t privateBit             : 1;
    uint8_t channelConfiguration   : 3;
    uint8_t originalCopy           : 1;
    uint8_t home                   : 1;
    uint8_t copyrightIDBit         : 1;
    uint8_t copyrightIDStart       : 1;
    uint16_t frameLength           : 13;
    uint16_t bufferFullness        : 11;
    uint8_t numRawDataBlocks       : 2;
};

struct ADIFHeader {
    uint32_t id                : 32;
    uint8_t copyrightIDPresent : 1;
    uint8_t copyrightID[9];
    uint8_t originalCopy             : 1;
    uint8_t home                     : 1;
    uint8_t bitStreamType            : 1;
    uint32_t bitRate                 : 23;
    uint8_t numProgramConfigElements : 4;
    uint32_t bufferFullness          : 20;
};

union AACHeader {
    ADTSHeader adts;
    ADIFHeader adif;
};

class AACExtractor : public ExtractorHelper, public AudioDecodeCallback, public NonCopyable {
public:
    explicit AACExtractor(DataSourceBase *source);

    virtual uint64_t getDurationMs() { return m_durationMs; }
    virtual off64_t getDataSize() { return m_dataSize; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual void readAudioRawData(AudioBuffer::AudioBufferPtr &bufPtr);
    virtual ~AACExtractor();

private:
    status_t init();

private:
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out);

private:
    DataSourceBase *m_dataSource;
    AudioDecode *m_decode;
    AudioCodecID m_audioCodecID;
    size_t m_decSize;
    std::vector<AudioBuffer::AudioBufferPtr> m_decBufVec;
    AudioBuffer::AudioBufferPtr m_decBuf;
    status_t m_initCheck;
    bool m_validFormat;
    uint64_t m_durationMs;
    off64_t m_dataOffset;
    size_t m_dataSize;
    AudioSpec m_audioSpec;
    AACHeader m_header;
};