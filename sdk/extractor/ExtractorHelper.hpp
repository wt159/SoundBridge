#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ByteUtils.h"
#include "DataSource.hpp"
#include "ErrorUtils.h"
#include <string>
#include <unordered_map>

enum standardExtractors {
    WAV_EXTRACTOR,
    MP3_EXTRACTOR,
    AAC_EXTRACTOR,
    FLAC_EXTRACTOR,
    OGG_EXTRACTOR,
    AIFF_EXTRACTOR,
    ASF_EXTRACTOR,
    M4A_EXTRACTOR,
    APE_EXTRACTOR,
    MKV_EXTRACTOR,
    OPUS_EXTRACTOR,
    UNKNOWN_EXTRACTOR
};

// clang-format off
static const std::unordered_map<std::string, standardExtractors> defaultExtractorMap = {
    { ".wav", WAV_EXTRACTOR },
    { ".aac", AAC_EXTRACTOR },
    { ".mp3", MP3_EXTRACTOR },
    { ".flac", FLAC_EXTRACTOR },
    { ".m4a", M4A_EXTRACTOR },
    { ".ogg", OGG_EXTRACTOR },
    { ".aiff", AIFF_EXTRACTOR },
    { ".asf", ASF_EXTRACTOR },
    { ".ape", APE_EXTRACTOR },
    { ".mkv", MKV_EXTRACTOR },
    { ".wma", ASF_EXTRACTOR },
    { ".amr", ASF_EXTRACTOR },
    { ".opus", OPUS_EXTRACTOR }
};
// clang-format on

class ExtractorHelper {
public:
    virtual ~ExtractorHelper() { }

    virtual const char *name() { return "<unspecified>"; }
    virtual sdk_utils::status_t initCheck()           = 0;
    virtual AudioSpec getAudioSpec()                  = 0;
    virtual AudioCodecID getAudioCodecID()            = 0;
    virtual AudioBuffer::AudioBufferPtr getMetaData() = 0;
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return nullptr; }
    virtual int getBitRate() { return 0; }
    virtual int getBlockAlign() { return 0; }
    virtual sdk_utils::status_t seekToMs(uint64_t targetMs, uint64_t *actualMs = nullptr)
    {
        (void)actualMs;
        (void)targetMs;
        return sdk_utils::INVALID_OPERATION;
    }
    virtual DataSourceBase *getDataSource() { return nullptr; }
    virtual off64_t getDataSize() { return 0; }
    virtual off64_t getAudioDataOffset() { return 0; }

protected:
    ExtractorHelper() { }

private:
    ExtractorHelper(const ExtractorHelper &);
    ExtractorHelper &operator=(const ExtractorHelper &);
};
