#include "ExtractorFactory.h"
#include "FileSource.h"
#include "WavExtractor.h"

ExtractorHelper *ExtractorFactory::createExtractor(DataSourceBase *source,
                                                   const std::string &extensionName)
{
    auto search = defaultExtractorMap.find(extensionName);
    if (search == defaultExtractorMap.end())
        return nullptr;
    switch (search->second) {
    case standardExtractors::WAV_EXTRACTOR:
        return new WAVExtractor(source);
    case standardExtractors::MP3_EXTRACTOR:
        break;
    default:
        break;
    }
    return nullptr;
}