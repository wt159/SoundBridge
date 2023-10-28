#include "ExtractorFactory.h"
#include "LogWrapper.h"
#include "FileSource.h"
#include "wav/WavExtractor.h"
#include "aac/AACExtractor.h"
#include "mp3/MP3Extractor.h"
#include "flac/FLACExtractor.h"
#include "m4a/M4AExtractor.h"
#include "ogg/OGGExtractor.h"
#include "aiff/AIFFExtractor.h"

#define LOG_TAG "ExtractorFactory"

ExtractorHelper *ExtractorFactory::createExtractor(DataSourceBase *source,
                                                   const std::string &extensionName)
{
    auto search = defaultExtractorMap.find(extensionName);
    if (search == defaultExtractorMap.end())
        return nullptr;
    switch (search->second) {
    case standardExtractors::WAV_EXTRACTOR:
        return new WAVExtractor(source);
    case standardExtractors::AAC_EXTRACTOR:
        return new AACExtractor(source);
    case standardExtractors::MP3_EXTRACTOR:
        return new MP3Extractor(source);
    case standardExtractors::FLAC_EXTRACTOR:
        return new FLACExtractor(source);
    case standardExtractors::M4A_EXTRACTOR:
        return new M4AExtractor(source);
    case standardExtractors::OGG_EXTRACTOR:
        return new OGGExtractor(source);
    case standardExtractors::AIFF_EXTRACTOR:
        return new AIFFExtractor(source);
    default:
        LOGW("Unknown extractor type");
        break;
    }
    return nullptr;
}