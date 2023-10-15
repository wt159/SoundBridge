#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ByteUtils.h"
#include "ExtractorApi.h"
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
    /* { ".aiff", AIFF_EXTRACTOR },
    { ".asf", ASF_EXTRACTOR },
    { ".wma", ASF_EXTRACTOR },
    { ".opus", OPUS_EXTRACTOR } */
};
// clang-format on

class ExtractorHelper {
public:
    virtual ~ExtractorHelper() { }

    virtual const char *name() { return "<unspecified>"; }
    virtual status_t initCheck()                      = 0;
    virtual AudioSpec getAudioSpec()                  = 0;
    virtual AudioCodecID getAudioCodecID()            = 0;
    virtual AudioBuffer::AudioBufferPtr getMetaData() = 0;

protected:
    ExtractorHelper() { }

private:
    ExtractorHelper(const ExtractorHelper &);
    ExtractorHelper &operator=(const ExtractorHelper &);
};