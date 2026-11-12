#include "ExtractorFactory.h"
#include "FileSource.h"
#include "LogWrapper.h"
#include "aac/AACExtractor.h"
#include "aiff/AIFFExtractor.h"
#include "ape/APEExtractor.h"
#include "asf/ASFExtractor.h"
#include "flac/FLACExtractor.h"
#include "hls/HlsExtractor.h"
#include "m4a/M4AExtractor.h"
#include "mkv/MKVExtractor.h"
#include "mp3/MP3Extractor.h"
#include "ogg/OGGExtractor.h"
#include "wav/WAVExtractor.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#define LOG_TAG "ExtractorFactory"

// 类内初始化的 static const 成员在被 odr-used（例如断言里按引用捕获）时需要类外定义。
const size_t ExtractorHelper::kUnknownPacketCount;

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
    uint8_t buf[16] = { 0 };
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
    const uint8_t asfGuid[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                  0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
    bool asfMatch             = true;
    for (int i = 0; i < 16; ++i) {
        if (buf[i] != asfGuid[i]) {
            asfMatch = false;
            break;
        }
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
    bool allowEmptySpec = false;
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

bool lookupEntry(const std::string &normalizedExtension, ExtractorEntry *entry)
{
    if (entry == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    RegistryMap &registry = extractorRegistry();
    RegistryMap::const_iterator search = registry.find(normalizedExtension);
    if (search == registry.end()) {
        return false;
    }
    *entry = search->second;
    return true;
}

// 规格 B2 / 10.#14：工厂出口是拒绝文件的唯一出口。
// initCheck != OK 与 spec 不合格都在这里收成 nullptr，调用方只判断返回值。
// allowEmptySpec 只给测试替身放行 spec 检查，init 失败一律拒绝。
ExtractorHelper *finalizeExtractor(ExtractorHelper *extractor, const ExtractorEntry &entry,
                                   const std::string &extensionName)
{
    if (extractor == nullptr) {
        return nullptr;
    }
    if (extractor->initCheck() != sdk_utils::OK) {
        LOGW("createExtractor init failed: %s", extensionName.c_str());
        delete extractor;
        return nullptr;
    }

    const AudioSpec spec = extractor->getAudioSpec();
    if (!entry.allowEmptySpec && (spec.sampleRate <= 0 || spec.numChannel <= 0)) {
        LOGW("createExtractor got unusable spec: %s (sr=%d ch=%d)", extensionName.c_str(),
             spec.sampleRate, spec.numChannel);
        delete extractor;
        return nullptr;
    }
    return extractor;
}

// 扩展名命中后构造一次；失败返回 nullptr（对象已释放）。
ExtractorHelper *createFromEntry(const ExtractorEntry &entry, DataSourceBase *source,
                                 const std::string &extensionName)
{
    // 观测点（规格第 5 节 / 票面 #56）：init 只读扫描耗时。
    // 提取器的构造就是 init（demux 装配 + 逐包装载），所以这里量构造耗时。
    // 只记录、不判红绿，也不参与工厂的接受/拒绝判断。
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    ExtractorHelper *extractor                        = entry.creator(source);
    const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - begin;
    if (extractor != nullptr) {
        extractor->setInitScanMicros(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
    }
    return finalizeExtractor(extractor, entry, extensionName);
}
} // namespace

bool ExtractorFactory::registerExtractor(const std::string &extensionName, ExtractorCreator creator)
{
    return registerExtractor(extensionName, std::move(creator), nullptr, false);
}

bool ExtractorFactory::registerExtractor(const std::string &extensionName, ExtractorCreator creator,
                                         ExtractorSniffer sniffer, bool allowEmptySpec)
{
    const std::string normalized = normalizeExtension(extensionName);
    if (normalized.empty() || !creator) {
        LOGW("registerExtractor ignore invalid extensionName/creator");
        return false;
    }

    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    auto &registry = extractorRegistry();
    ExtractorEntry entry;
    entry.creator        = std::move(creator);
    entry.sniffer        = std::move(sniffer);
    entry.allowEmptySpec = allowEmptySpec;
    std::pair<RegistryMap::iterator, bool> insertResult
        = registry.emplace(normalized, std::move(entry));
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
    ExtractorEntry registeredEntry;
    if (lookupEntry(normalized, &registeredEntry)) {
        ExtractorHelper *extractor = createFromEntry(registeredEntry, source, normalized);
        if (extractor != nullptr) {
            return extractor;
        }

        // 规格 5.4 H4：扩展名命中但头坏了，做一次魔数回退，命中即用、不递归。
        // 否则「改成不认识的后缀能播、改成认识的后缀判损坏」无法向用户解释。
        if (!enableSniff) {
            LOGW("createExtractor rejected: %s", normalized.c_str());
            return nullptr;
        }
        const std::string sniffed = sniffExtensionByMagic(source);
        if (!sniffed.empty() && sniffed != normalized) {
            ExtractorEntry sniffedEntry;
            if (lookupEntry(sniffed, &sniffedEntry)) {
                ExtractorHelper *fallback = createFromEntry(sniffedEntry, source, sniffed);
                if (fallback != nullptr) {
                    LOGI("createExtractor magic fallback: %s -> %s", normalized.c_str(),
                         sniffed.c_str());
                    return fallback;
                }
            }
        }
        LOGW("createExtractor rejected: %s", normalized.c_str());
        return nullptr;
    }

    if (!enableSniff) {
        LOGW("createExtractor unsupported extension: %s", normalized.c_str());
        return nullptr;
    }

    const std::string sniffed = sniffExtensionByMagic(source);
    if (!sniffed.empty()) {
        ExtractorEntry sniffedEntry;
        if (lookupEntry(sniffed, &sniffedEntry)) {
            LOGI("createExtractor sniff hit: %s", sniffed.c_str());
            return createFromEntry(sniffedEntry, source, sniffed);
        }
        LOGW("createExtractor unsupported sniffed extension: %s", sniffed.c_str());
    }

    std::vector<std::pair<std::string, ExtractorEntry>> entries;
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
            return createFromEntry(entry.second, source, entry.first);
        }
    }

    LOGW("createExtractor sniff failed: %s", normalized.c_str());
    return nullptr;
}

bool ExtractorFactory::isSupportedExtension(const std::string &extensionName)
{
    const std::string normalized = normalizeExtension(extensionName);
    if (normalized.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> guard(extractorRegistryMutex());
    auto &registry = extractorRegistry();
    return registry.find(normalized) != registry.end();
}

std::vector<std::string> ExtractorFactory::registeredExtensions()
{
    std::vector<std::string> extensions;
    {
        std::lock_guard<std::mutex> guard(extractorRegistryMutex());
        auto &registry = extractorRegistry();
        extensions.reserve(registry.size());
        for (const auto &entry : registry) {
            extensions.push_back(entry.first);
        }
    }
    std::sort(extensions.begin(), extensions.end());
    return extensions;
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
// P2-1（issue #144 / #143 契约 §2）：HLS VOD 直通。只注册进工厂注册表、
// 不进 defaultExtractorMap（FileSearch 本地扫描白名单不动，受支持格式清单不扩）。
// 无 sniff：URL 直通分派在 MusicPlayList::_addMusic 按清单内容判定，扩展名仅作出口。
REGISTER_EXTRACTOR(".m3u8", HlsExtractor);
