#include "ExtractorFactory.h"
#include "LogWrapper.h"
#include "FileSource.h"
#include "aac/AACExtractor.h"
#include "aiff/AIFFExtractor.h"
#include "asf/ASFExtractor.h"
#include "flac/FLACExtractor.h"
#include "m4a/M4AExtractor.h"
#include "mp3/MP3Extractor.h"
#include "ogg/OGGExtractor.h"
#include "wav/WAVExtractor.h"
#include <mutex>
#include <unordered_map>

#define LOG_TAG "ExtractorFactory"

namespace {
using RegistryMap = std::unordered_map<std::string, ExtractorFactory::ExtractorCreator>;

RegistryMap &extractorRegistry()
{
    static RegistryMap s_registry;
    return s_registry;
}

std::mutex &extractorRegistryMutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}
}

bool ExtractorFactory::registerExtractor(const std::string &extensionName, ExtractorCreator creator)
{
    // 避免空扩展名或空 creator 被加入注册表，防止 create 时崩溃。
    if (extensionName.empty() || !creator) {
        LOGW("registerExtractor ignore invalid extensionName/creator");
        return false;
    }

    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    auto &registry = extractorRegistry();
    std::pair<RegistryMap::iterator, bool> insertResult =
        registry.emplace(extensionName, std::move(creator));
    if (!insertResult.second) {
        LOGW("registerExtractor duplicate extension: %s", extensionName.c_str());
        return false;
    }
    return true;
}

ExtractorHelper *ExtractorFactory::createExtractor(DataSourceBase *source,
                                                   const std::string &extensionName)
{
    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    auto &registry = extractorRegistry();
    auto search = registry.find(extensionName);
    if (search == registry.end()) {
        LOGW("createExtractor unsupported extension: %s", extensionName.c_str());
        return nullptr;
    }

    return search->second(source);
}

// 方向一（插件化与自动注册）的第一步：
// 把“格式->构造器”从 switch 迁移成“静态注册表”。
REGISTER_EXTRACTOR(".wav", WAVExtractor);
REGISTER_EXTRACTOR(".aac", AACExtractor);
REGISTER_EXTRACTOR(".mp3", MP3Extractor);
REGISTER_EXTRACTOR(".flac", FLACExtractor);
REGISTER_EXTRACTOR(".m4a", M4AExtractor);
REGISTER_EXTRACTOR(".ogg", OGGExtractor);
REGISTER_EXTRACTOR(".aiff", AIFFExtractor);
REGISTER_EXTRACTOR(".asf", ASFExtractor);
REGISTER_EXTRACTOR(".wma", ASFExtractor);
REGISTER_EXTRACTOR(".amr", ASFExtractor);
