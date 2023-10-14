#include "ExtractorFactory.h"
#include "FileSource.h"
#include "wav/WavExtractor.h"
#include "aac/AACExtractor.h"
#include "mp3/MP3Extractor.h"
#include "flac/FLACExtractor.h"

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
    default:
        break;
    }
    return nullptr;
}