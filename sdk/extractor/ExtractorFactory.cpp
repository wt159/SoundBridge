#include "ExtractorFactory.h"
#include "LogWrapper.h"
#include "FileSource.h"
#include "aac/AACExtractor.h"
#include "aiff/AIFFExtractor.h"
#include "ape/APEExtractor.h"
#include "asf/ASFExtractor.h"
#include "flac/FLACExtractor.h"
#include "m4a/M4AExtractor.h"
#include "mkv/MKVExtractor.h"
#include "mp3/MP3Extractor.h"
#include "ogg/OGGExtractor.h"
#include "wav/WAVExtractor.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <utility>

#define LOG_TAG "ExtractorFactory"

namespace {

std::string normalizeExtension(const std::string &extensionName)
{
    if (extensionName.empty()) {
        return std::string();
    }
    std::string out = extensionName;
    if (out[0] != '.') {
        out = "." + out;
    }
    for (char &ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    const ssize_t n = source->readAt(0, buf, size);
    return n >= static_cast<ssize_t>(size);
}

bool matchAscii(const uint8_t *buf, size_t offset, const char *text)
{
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (buf[offset + i] != static_cast<uint8_t>(text[i])) {
            return false;
        }
    }
    return true;
}

std::string sniffExtensionByMagic(DataSourceBase *source)
{
    uint8_t buf[16] = {0};
    if (!readHeader(source, buf, sizeof(buf))) {
        return std::string();
    }

    if (matchAscii(buf, 0, "RIFF") && matchAscii(buf, 8, "WAVE")) {
        return ".wav";
    }
    if (matchAscii(buf, 0, "FORM") && (matchAscii(buf, 8, "AIFF") || matchAscii(buf, 8, "AIFC"))) {
        return ".aiff";
    }
    if (matchAscii(buf, 0, "fLaC")) {
        return ".flac";
    }
    if (matchAscii(buf, 0, "OggS")) {
        return ".ogg";
    }
    if (matchAscii(buf, 0, "ID3")) {
        return ".mp3";
    }
    if (matchAscii(buf, 0, "MAC ")) {
        return ".ape";
    }
    // MKV/WEBM EBML header
    if (buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3) {
        return ".mkv";
    }
    if (buf[0] == 0xFF && (buf[1] & 0xF0) == 0xF0) {
        return ".aac"; // ADTS
    }
    // ASF/WMA magic GUID
    const uint8_t asfGuid[16] = {0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
    bool asfMatch = true;
    for (int i = 0; i < 16; ++i) {
        if (buf[i] != asfGuid[i]) { asfMatch = false; break; }
    }
    if (asfMatch) {
        return ".asf";
    }
    if (matchAscii(buf, 4, "ftyp")) {
        return ".m4a"; // treat mp4 container as m4a
    }
    if (matchAscii(buf, 0, "#!AMR")) {
        return ".amr";
    }
    return std::string();
}

} // namespace
namespace {
struct ExtractorEntry {
    ExtractorFactory::ExtractorCreator creator;
    ExtractorFactory::ExtractorSniffer sniffer;
};

using RegistryMap = std::unordered_map<std::string, ExtractorEntry>;

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
    return registerExtractor(extensionName, std::move(creator), nullptr);
}

bool ExtractorFactory::registerExtractor(const std::string &extensionName,
                                         ExtractorCreator creator,
                                         ExtractorSniffer sniffer)
{
    const std::string normalized = normalizeExtension(extensionName);
    if (normalized.empty() || !creator) {
        LOGW("registerExtractor ignore invalid extensionName/creator");
        return false;
    }

    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    auto &registry = extractorRegistry();
    ExtractorEntry entry;
    entry.creator = std::move(creator);
    entry.sniffer = std::move(sniffer);
    std::pair<RegistryMap::iterator, bool> insertResult = registry.emplace(normalized, std::move(entry));
    if (!insertResult.second) {
        LOGW("registerExtractor duplicate extension: %s", normalized.c_str());
        return false;
    }
    return true;
}

ExtractorHelper *ExtractorFactory::createExtractor(DataSourceBase *source,
                                                   const std::string &extensionName)
{
    return createExtractor(source, extensionName, false);
}

ExtractorHelper *ExtractorFactory::createExtractor(DataSourceBase *source,
                                                   const std::string &extensionName,
                                                   bool enableSniff)
{
    const std::string normalized = normalizeExtension(extensionName);
    {
        std::lock_guard<std::mutex> guard(extractorRegistryMutex());
        auto &registry = extractorRegistry();
        auto search = registry.find(normalized);
        if (search != registry.end()) {
            return search->second.creator(source);
        }
    }

    if (!enableSniff) {
        LOGW("createExtractor unsupported extension: %s", normalized.c_str());
        return nullptr;
    }

    const std::string sniffed = sniffExtensionByMagic(source);
    if (!sniffed.empty()) {
        std::lock_guard<std::mutex> guard(extractorRegistryMutex());
        auto &registry = extractorRegistry();
        auto search = registry.find(sniffed);
        if (search != registry.end()) {
            LOGI("createExtractor sniff hit: %s", sniffed.c_str());
            return search->second.creator(source);
        }
        LOGW("createExtractor unsupported sniffed extension: %s", sniffed.c_str());
    }

    std::vector<std::pair<std::string, ExtractorEntry> > entries;
    {
        std::lock_guard<std::mutex> guard(extractorRegistryMutex());
        auto &registry = extractorRegistry();
        entries.reserve(registry.size());
        for (const auto &it : registry) {
            entries.push_back(it);
        }
    }

    for (const auto &entry : entries) {
        if (!entry.second.sniffer) {
            continue;
        }
        if (entry.second.sniffer(source)) {
            LOGI("createExtractor sniffer hit: %s", entry.first.c_str());
            return entry.second.creator(source);
        }
    }

    LOGW("createExtractor sniff failed: %s", normalized.c_str());
    return nullptr;
}

// Register built-in extractors, enabling sniffing when possible.
REGISTER_EXTRACTOR_WITH_SNIFF(".wav", WAVExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".aac", AACExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".mp3", MP3Extractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".flac", FLACExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".m4a", M4AExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".ogg", OGGExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".aiff", AIFFExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".asf", ASFExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".ape", APEExtractor);
REGISTER_EXTRACTOR_WITH_SNIFF(".mkv", MKVExtractor);
REGISTER_EXTRACTOR(".wma", ASFExtractor);
REGISTER_EXTRACTOR(".amr", ASFExtractor);









