#pragma once
#include "ExtractorHelper.hpp"
#include "DataSource.hpp"
#include "NonCopyable.hpp"

class ExtractorFactory : public NonCopyable
{
public:
    ExtractorFactory() = delete;
    ~ExtractorFactory() = delete;

    static ExtractorHelper *createExtractor(DataSourceBase *source, const std::string &extensionName);
};
