#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ErrorCode.hpp>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <MusicPlayList.h>
#include <MusicPlayer.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
AudioSpec makeDevSpec()
{
    AudioSpec spec;
    spec.sampleRate     = 44100;
    spec.numChannel     = 2;
    spec.bitsPerSample  = 16;
    spec.bytesPerSample = 2;
    spec.format         = AudioFormatS16;
    spec.samples        = 0;
    spec.durationMs     = 0;
    return spec;
}

// 坏输入素材一律字节手术合成到系统临时目录，不入库；析构时删干净（规格 10.#25）。
// 读源走 FileSource，和生产线同一条路径解析（中文名素材在 Windows 上只能这样读）。
class TempFileSet {
public:
    explicit TempFileSet(const char *tag)
        : m_tag(tag)
    {
        const char *base = std::getenv("TEMP");
        if (base == nullptr || base[0] == '\0') {
            base = std::getenv("TMPDIR");
        }
        m_dir = (base != nullptr && base[0] != '\0') ? std::string(base) : std::string(".");
#ifdef _WIN32
        const int pid = static_cast<int>(_getpid());
#else
        const int pid = static_cast<int>(getpid());
#endif
        m_prefix = "sb_badinput_" + std::to_string(pid) + "_" + m_tag + "_";
    }

    ~TempFileSet()
    {
        for (const std::string &path : m_files) {
            std::remove(path.c_str());
        }
        for (const std::string &dir : m_dirs) {
            std::remove(dir.c_str());
        }
    }

    std::string make(const char *extension)
    {
        std::string path = m_dir + "/" + m_prefix + std::to_string(++s_counter)
            + extension;
        m_files.push_back(path);
        return path;
    }

    std::string makeDir()
    {
        std::string path = m_dir + "/" + m_prefix + "dir" + std::to_string(++s_counter);
#ifdef _WIN32
        _mkdir(path.c_str());
#else
        mkdir(path.c_str(), 0700);
#endif
        m_dirs.push_back(path);
        return path;
    }

private:
    static int s_counter;
    std::string m_prefix;
    std::string m_dir;
    std::string m_tag;
    std::vector<std::string> m_files;
    std::vector<std::string> m_dirs;
};

int TempFileSet::s_counter = 0;

std::vector<char> readWholeFile(const std::string &path)
{
    std::vector<char> data;
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return data;
    }
    off64_t size = 0;
    if (source.getSize(&size) != sdk_utils::OK || size <= 0) {
        return data;
    }
    data.resize(static_cast<size_t>(size));
    ssize_t got = source.readAt(0, data.data(), data.size());
    if (got != static_cast<ssize_t>(data.size())) {
        data.clear();
    }
    return data;
}

bool writeWholeFile(const std::string &path, const std::vector<char> &data)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

// 把音频中间一段等长区间归零：长度不变，模拟「流内坏帧」（规格 4 素材清单）。
void zeroRange(std::vector<char> &data, size_t offset, size_t length)
{
    for (size_t i = 0; i < length && offset + i < data.size(); ++i) {
        data[offset + i] = 0;
    }
}

struct DrainResult {
    size_t bytes                = 0;
    StreamDecoderState endState = StreamDecoderState::IDLE;
    bool settled                = false;
};

// 判据走流式播放路径：start() + 消费 ring（规格基石 B4）。
DrainResult drainUntilSettled(AudioStreamDecoder &decoder, AudioRingBuffer &ring, int timeoutMs)
{
    DrainResult result;
    std::vector<char> buf(4096);
    const int pollMs = 5;
    int elapsed      = 0;
    while (elapsed <= timeoutMs) {
        while (ring.availableRead() > 0) {
            size_t once = std::min(buf.size(), ring.availableRead());
            size_t got  = ring.read(buf.data(), once);
            if (got == 0) {
                break;
            }
            result.bytes += got;
        }
        StreamDecoderState state = decoder.state();
        if (state == StreamDecoderState::ERROR || state == StreamDecoderState::EOS) {
            result.settled  = true;
            result.endState = state;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    result.endState = decoder.state();
    return result;
}

// 记录全部上报，不用「只留最后一条」的 mock：汇总是批次末尾才发的，会被后续事件覆盖。
struct RecordingPlayerListener : public sdk::MusicPlayerListener {
    struct Report {
        sdk::ErrorCode code;
        std::string detail;
        int index;
        std::string path;
    };

    mutable std::mutex mutex;
    std::vector<Report> reports;

    void onMusicPlayerStateChanged(sdk::MusicPlayerState) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++stateChanges;
    }
    void onMusicPlayerListCurrentIndexChanged(int) override { }
    void onMusicPlayerDurationChanged(uint64_t) override { }
    void onMusicPlayerPositionChanged(uint64_t) override { }
    void onMusicPlayerMusicListChanged(std::list<sdk::MusicIndex>) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++listChanges;
    }
    void onMusicPlayerError(sdk::ErrorCode code, const std::string &detail, int index,
                            const std::string &path, const std::string &) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        reports.push_back(Report { code, detail, index, path });
    }

    size_t errorCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return reports.size();
    }

    std::vector<Report> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return reports;
    }

    int stateChanges = 0;
    int listChanges  = 0;
};

struct RecordingPlayListCallback : public sdk::MusicPlayListCallback {
    struct ErrorRecord {
        sdk::ErrorCode code;
        std::string detail;
        int index;
        std::string path;
    };

    int currentBufCount = 0;
    int lastCurrentIndex = -1;
    std::vector<ErrorRecord> errors;

    void putMusicPlayListCurBuf(sdk::MusicPropertiesPtr property) override
    {
        ++currentBufCount;
        lastCurrentIndex = property ? static_cast<int>(property->index) : -1;
    }
    void updateMusicList(std::vector<sdk::MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(sdk::ErrorCode code, const std::string &detail, int index,
                              const std::string &path) override
    {
        errors.push_back(ErrorRecord { code, detail, index, path });
    }
};

std::shared_ptr<sdk::MusicPlayList> makePlayList(RecordingPlayListCallback &callback,
                                                 const std::vector<std::string> &paths)
{
    static WorkQueue workQueue;
    AudioSpec devSpec = makeDevSpec();
    auto playlist     = std::make_shared<sdk::MusicPlayList>(&callback, &workQueue, devSpec);
    for (const std::string &path : paths) {
        std::promise<void> done;
        playlist->addMusicWithNotify(path, [&done]() { done.set_value(); });
        done.get_future().wait();
    }
    return playlist;
}

template <typename Predicate> bool waitFor(Predicate predicate, int timeoutMs)
{
    int elapsed = 0;
    while (elapsed <= timeoutMs) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        elapsed += 20;
    }
    return false;
}
// ---- 以下 helper 来自 #35（进程安全票）的用例基建 ----

void clearBits(std::vector<char> &data, size_t offset, unsigned char mask)
{
    if (offset < data.size()) {
        data[offset] = static_cast<char>(static_cast<unsigned char>(data[offset]) & ~mask);
    }
}

struct ExtractorOutcome {
    bool created    = false;
    bool initOk     = false;
    int sampleRate  = 0;
    int numChannel  = 0;
    uint64_t durationMs = 0;
};

// 与生产线同一个出口：createExtractor(enableSniff=true) 拿到提取器，再看 initCheck()。
ExtractorOutcome openExtractor(const std::string &path, const char *extension)
{
    ExtractorOutcome outcome;
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return outcome;
    }
    std::unique_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(&source, extension, true));
    if (!extractor) {
        return outcome;
    }
    outcome.created = true;
    outcome.initOk  = extractor->initCheck() == sdk_utils::OK;
    // getAudioSpec() 不在禁止调用的数据接口之列（H5 禁的是 metadata / seek / 数据长度）。
    AudioSpec spec      = extractor->getAudioSpec();
    outcome.sampleRate  = spec.sampleRate;
    outcome.numChannel  = spec.numChannel;
    outcome.durationMs  = spec.durationMs;
    return outcome;
}

// 「拒绝」的判据：工厂自己返回 nullptr，或提取器 init 失败。
// 出口的 spec 校验（规格 10.#14）落地前工厂仍会返回对象，所以两种形态都要认。
bool rejected(const ExtractorOutcome &outcome)
{
    return !outcome.created || !outcome.initOk;
}

// #38：直接探工厂出口契约的替身。真实素材要么 init 失败、要么 spec 自洽，
// 没法单独造出「init 成功但 spec 为空」这一种状态，所以用替身精确锁住出口。
class FixedSpecProbeExtractor : public ExtractorHelper {
public:
    FixedSpecProbeExtractor(int sampleRate, int numChannel, sdk_utils::status_t initStatus)
        : m_sampleRate(sampleRate)
        , m_numChannel(numChannel)
        , m_initStatus(initStatus)
    {
    }

    sdk_utils::status_t initCheck() override { return m_initStatus; }
    AudioSpec getAudioSpec() override
    {
        AudioSpec spec;
        spec.sampleRate = m_sampleRate;
        spec.numChannel = m_numChannel;
        return spec;
    }
    AudioCodecID getAudioCodecID() override { return AUDIO_CODEC_ID_NONE; }

private:
    int m_sampleRate;
    int m_numChannel;
    sdk_utils::status_t m_initStatus;
};

long long elapsedMs(std::chrono::steady_clock::time_point begin)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

long long fileBytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return 0;
    }
    return static_cast<long long>(in.tellg());
}

// 日志目录的总字节数。坏文件让解析循环原地打转时，日志会以每秒数万行的速度增长，
// 所以「日志有上界」可以用目录增长量来判（规格 10.#3 的日志洪水）。
long long logDirBytes(const std::string &dir)
{
    long long total = 0;
#ifdef _WIN32
    std::string pattern = dir + "/*.log";
    struct _finddata_t entry;
    intptr_t handle = _findfirst(pattern.c_str(), &entry);
    if (handle == -1) {
        return 0;
    }
    do {
        total += fileBytes(dir + "/" + entry.name);
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return 0;
    }
    while (struct dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        total += fileBytes(dir + "/" + entry->d_name);
    }
    closedir(handle);
#endif
    return total;
}

// unit 测试把日志写在 cwd 下的 ./log（见 UnitTestsMain.cpp）。
const char *kLogDir = "./log";

// ---- 以下 helper 来自 #36（截断票）----

// 一次流式播放的观测结果。判据只看这里的 bytes（= 用户听得到的字节），
// 不读坏文件自报的任何时长（规格 3.1：D 类的声明会跟着文件一起缩水）。
struct StreamMeasurement {
    bool opened              = false;
    size_t bytes             = 0;
    StreamDecoderState state = StreamDecoderState::IDLE;
    sdk::ErrorCode code      = sdk::ErrorCode::Ok;
    std::string detail;
};

// 直接喂一个现成的提取器：用例要控制 device spec 或替换声明层时走这里。
StreamMeasurement measureStreamWithExtractor(const std::shared_ptr<ExtractorHelper> &extractor,
                                             const AudioSpec &devSpec, int timeoutMs)
{
    StreamMeasurement measurement;
    if (!extractor || extractor->initCheck() != sdk_utils::OK) {
        return measurement;
    }
    measurement.opened = true;

    AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
    AudioStreamDecoder decoder(&ring, devSpec);
    if (decoder.start(extractor.get()) != sdk_utils::OK) {
        measurement.state = decoder.state();
        measurement.code  = decoder.errorCode();
        return measurement;
    }
    DrainResult drained = drainUntilSettled(decoder, ring, timeoutMs);
    decoder.stop();
    measurement.bytes  = drained.bytes;
    measurement.state  = drained.endState;
    measurement.code   = decoder.errorCode();
    measurement.detail = decoder.errorDetail();
    return measurement;
}

StreamMeasurement measureStream(const std::string &path, const char *extension, int timeoutMs)
{
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return StreamMeasurement();
    }
    // source 的生命周期覆盖整个解码过程：提取器只持它的裸指针。
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(&source, extension, true));
    return measureStreamWithExtractor(extractor, makeDevSpec(), timeoutMs);
}

// ring 里是设备格式（makeDevSpec）的字节，换算成毫秒。
uint64_t devBytesToMs(size_t bytes)
{
    AudioSpec spec       = makeDevSpec();
    const uint64_t denom = static_cast<uint64_t>(spec.sampleRate) * spec.numChannel
        * spec.bytesPerSample;
    return (denom == 0) ? 0 : static_cast<uint64_t>(bytes) * 1000 / denom;
}

// 规格 T6：带宽 ±max(5% × good, 150ms)。1% 会假红——实测「字节比例 → 时长比例」
// 最大偏 2.07pp（flac r=0.90）。
uint64_t truncationBandMs(uint64_t goodMs)
{
    const uint64_t relative = goodMs * 5 / 100;
    return relative > 150 ? relative : 150;
}

bool withinTruncationBand(uint64_t measuredMs, uint64_t expectMs, uint64_t goodMs)
{
    const uint64_t band = truncationBandMs(goodMs);
    return measuredMs + band >= expectMs && measuredMs <= expectMs + band;
}

// 保留头部、砍掉尾部：r 用百分比，避免浮点累积误差进了容差。
std::vector<char> keepPercent(const std::vector<char> &data, int percent)
{
    return std::vector<char>(data.begin(), data.begin() + data.size() * percent / 100);
}

std::vector<char> dropTailBytes(const std::vector<char> &data, size_t dropBytes)
{
    if (dropBytes >= data.size()) {
        return std::vector<char>();
    }
    return std::vector<char>(data.begin(), data.end() - static_cast<long>(dropBytes));
}

// ---- 以下 helper 来自 #37（坏帧：跳过继续）----

// 只数「跳过」通知的极小 callback：护栏用例直接驱动 AudioDecode，不需要 PCM。
struct SkipCountingCallback : public AudioDecodeCallback {
    int skips = 0;
    void onAudioDecodeCallback(AudioDecodeSpec &) override { }
    void onAudioDecodeSkip() override { ++skips; }
};

struct BadFrameMeasurement {
    bool opened              = false;
    uint64_t goodMs          = 0;
    uint64_t measuredMs      = 0;
    size_t measuredBytes     = 0;
    StreamDecoderState state = StreamDecoderState::IDLE;
    sdk::ErrorCode code      = sdk::ErrorCode::Ok;
    std::string detail;
};

// 规格 4.2 的实测方法：在文件 50% 处零化一段**等长**区间（长度不变），
// 所以「文件变短了」不可能成为旁证；再走同一条流式路径测「完好」与「坏帧」两次。
// 判据只看 `measuredMs ≈ goodMs`（规格 4.1 的「坏帧之后还有数据」）与报了什么码。
BadFrameMeasurement measureMidFrameCorruption(const std::string &goodPath, const char *extension,
                                             size_t zeroBytes, int timeoutMs, const char *tag)
{
    BadFrameMeasurement result;
    std::vector<char> good = readWholeFile(goodPath);
    if (good.size() <= zeroBytes * 2) {
        return result;
    }

    StreamMeasurement goodRun = measureStream(goodPath, extension, timeoutMs);
    if (!goodRun.opened || goodRun.bytes == 0) {
        return result;
    }
    result.goodMs = devBytesToMs(goodRun.bytes);
    if (result.goodMs == 0) {
        return result;
    }

    std::vector<char> broken = good;
    zeroRange(broken, broken.size() / 2, zeroBytes);

    TempFileSet tmp(tag);
    std::string badPath = tmp.make(extension);
    if (!writeWholeFile(badPath, broken)) {
        return result;
    }

    StreamMeasurement bad = measureStream(badPath, extension, timeoutMs);
    result.opened        = true;
    result.measuredMs    = devBytesToMs(bad.bytes);
    result.measuredBytes = bad.bytes;
    result.state         = bad.state;
    result.code          = bad.code;
    result.detail        = bad.detail;
    return result;
}

// ---- 以下 helper 来自 #39（时长来源前移）----

// 素材手术只碰固定偏移，所以只留最小的一组定宽读写。
uint32_t readU32BE(const std::vector<char> &data, size_t offset)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) << 24)
        | (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8)
        | static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

uint64_t readU64BE(const std::vector<char> &data, size_t offset)
{
    return (static_cast<uint64_t>(readU32BE(data, offset)) << 32)
        | readU32BE(data, offset + 4);
}

void writeU32BE(std::vector<char> &data, size_t offset, uint32_t value)
{
    data[offset]     = static_cast<char>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<char>(value & 0xFF);
}

void writeU64BE(std::vector<char> &data, size_t offset, uint64_t value)
{
    writeU32BE(data, offset, static_cast<uint32_t>(value >> 32));
    writeU32BE(data, offset + 4, static_cast<uint32_t>(value & 0xFFFFFFFFull));
}

uint32_t readU32LE(const std::vector<char> &data, size_t offset)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(data[offset]))
        | (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

uint64_t readU64LE(const std::vector<char> &data, size_t offset)
{
    return static_cast<uint64_t>(readU32LE(data, offset))
        | (static_cast<uint64_t>(readU32LE(data, offset + 4)) << 32);
}

void writeU32LE(std::vector<char> &data, size_t offset, uint32_t value)
{
    data[offset]     = static_cast<char>(value & 0xFF);
    data[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
    data[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
    data[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

void writeU64LE(std::vector<char> &data, size_t offset, uint64_t value)
{
    writeU32LE(data, offset, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    writeU32LE(data, offset + 4, static_cast<uint32_t>(value >> 32));
}

// 在 mp4 的容器层级里找 mvhd / mdhd，把 duration ×2（长度不变）。
// 只递归 moov/trak/mdia，mdat 里的随机字节不会被误伤。
bool patchMp4DurationIn(std::vector<char> &data, size_t start, size_t end, const char *type)
{
    size_t offset = start;
    while (offset + 8 <= end) {
        uint64_t size   = readU32BE(data, offset);
        const char *tag = data.data() + offset + 4;
        size_t header   = 8;
        if (size == 1) {
            if (offset + 16 > end) {
                return false;
            }
            size   = readU64BE(data, offset + 8);
            header = 16;
        } else if (size == 0) {
            size = end - offset;
        }
        if (size < header || size > end - offset) {
            return false;
        }
        if (std::memcmp(tag, type, 4) == 0) {
            const size_t body    = offset + header;
            const unsigned char version = static_cast<unsigned char>(data[body]);
            // version 0：timescale@12 duration@16（32 位）；version 1：duration@24（64 位）。
            if (version == 0 && body + 20 <= offset + size) {
                writeU32BE(data, body + 16, readU32BE(data, body + 16) * 2);
                return true;
            }
            if (version == 1 && body + 32 <= offset + size) {
                writeU64BE(data, body + 24, readU64BE(data, body + 24) * 2);
                return true;
            }
            return false;
        }
        if (std::memcmp(tag, "moov", 4) == 0 || std::memcmp(tag, "trak", 4) == 0
            || std::memcmp(tag, "mdia", 4) == 0) {
            if (patchMp4DurationIn(data, offset + header, offset + static_cast<size_t>(size), type)) {
                return true;
            }
        }
        offset += static_cast<size_t>(size);
    }
    return false;
}

bool patchMp4Durations(std::vector<char> &data)
{
    return patchMp4DurationIn(data, 0, data.size(), "mvhd")
        && patchMp4DurationIn(data, 0, data.size(), "mdhd");
}

// ASF File Properties 对象的 playDuration ×2（100ns 单位，偏移 40 在对象体里）。
bool patchAsfPlayDuration(std::vector<char> &data)
{
    static const unsigned char kFilePropertiesGuid[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9,
                                                           0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0,
                                                           0x0C, 0x20, 0x53, 0x65 };
    for (size_t offset = 0; offset + 16 <= data.size(); ++offset) {
        if (std::memcmp(data.data() + offset, kFilePropertiesGuid, 16) != 0) {
            continue;
        }
        const size_t playDuration = offset + 24 + 40;
        if (playDuration + 8 > data.size()) {
            return false;
        }
        writeU64LE(data, playDuration, readU64LE(data, playDuration) * 2);
        return true;
    }
    return false;
}

// FLAC 的 STREAMINFO：sr(20) ch(3) bps(5) totalSamples(36) 挤在字节 10..17，
// totalSamples 是这 64 位的低 36 位。×2（长度不变）。
bool patchFlacTotalSamples(std::vector<char> &data)
{
    if (data.size() < 26 || std::memcmp(data.data(), "fLaC", 4) != 0) {
        return false;
    }
    const size_t field      = 8 + 10; // "fLaC" + 4 字节块头，首块必是 STREAMINFO
    const uint64_t kLow36   = 0xFFFFFFFFFull;
    uint64_t packed         = readU64BE(data, field);
    const uint64_t samples  = packed & kLow36;
    if (samples == 0) {
        return false;
    }
    writeU64BE(data, field, (packed & ~kLow36) | ((samples * 2) & kLow36));
    return true;
}

// wav 的 fmt 块：sampleRate 在块体 +4，byteRate 在 +8（声道×位宽×采样率，同步改）。
bool patchWavSampleRate(std::vector<char> &data, uint32_t sampleRate)
{
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0) {
        return false;
    }
    size_t offset = 12;
    while (offset + 8 <= data.size()) {
        const uint32_t chunkSize = readU32LE(data, offset + 4);
        if (std::memcmp(data.data() + offset, "fmt ", 4) == 0) {
            const size_t body = offset + 8;
            if (body + 12 > data.size()) {
                return false;
            }
            const uint32_t oldRate  = readU32LE(data, body + 4);
            const uint32_t byteRate = readU32LE(data, body + 8);
            if (oldRate == 0) {
                return false;
            }
            writeU32LE(data, body + 4, sampleRate);
            writeU32LE(data, body + 8, byteRate / oldRate * sampleRate);
            return true;
        }
        offset += 8 + static_cast<size_t>(chunkSize) + (chunkSize & 1);
    }
    return false;
}

// 声明层的替身：所有数据接口都转发给真实提取器，只把 AudioSpec 换成谎报值。
// 用来精确造出「声明值恰好等于 device spec」这一种状态——真实素材里
// 提取器和码流同源，造不出来。
class LyingSpecExtractor : public ExtractorHelper {
public:
    LyingSpecExtractor(const std::shared_ptr<ExtractorHelper> &inner, const AudioSpec &lie)
        : m_inner(inner)
        , m_lie(lie)
    {
    }

    sdk_utils::status_t initCheck() override { return m_inner->initCheck(); }
    AudioSpec getAudioSpec() override { return m_lie; }
    AudioCodecID getAudioCodecID() override { return m_inner->getAudioCodecID(); }
    std::vector<AudioBuffer::AudioBufferPtr> getPacketizedMetaData() const override
    {
        return m_inner->getPacketizedMetaData();
    }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_inner->getMetaData(); }
    AudioBuffer::AudioBufferPtr getCodecExtraData() override
    {
        return m_inner->getCodecExtraData();
    }
    int getBitRate() override { return m_inner->getBitRate(); }
    int getBlockAlign() override { return m_inner->getBlockAlign(); }
    DataSourceBase *getDataSource() override { return m_inner->getDataSource(); }
    off64_t getDataSize() override { return m_inner->getDataSize(); }
    off64_t getAudioDataOffset() override { return m_inner->getAudioDataOffset(); }

private:
    std::shared_ptr<ExtractorHelper> m_inner;
    AudioSpec m_lie;
};

// 一次「改声明字段前 / 后」的对照观测。
struct DurationLieMeasurement {
    bool opened          = false;
    uint64_t declaredMs  = 0;
    uint64_t decodedMs   = 0;
    size_t decodedBytes  = 0;
    StreamDecoderState state = StreamDecoderState::IDLE;
    sdk::ErrorCode code      = sdk::ErrorCode::Ok;
};

DurationLieMeasurement measureDuration(const std::string &path, const char *extension,
                                       int timeoutMs)
{
    DurationLieMeasurement result;
    ExtractorOutcome outcome = openExtractor(path, extension);
    if (!outcome.created || !outcome.initOk) {
        return result;
    }
    result.opened     = true;
    result.declaredMs = outcome.durationMs;

    StreamMeasurement run = measureStream(path, extension, timeoutMs);
    if (!run.opened) {
        result.opened = false;
        return result;
    }
    result.decodedBytes = run.bytes;
    result.decodedMs    = devBytesToMs(run.bytes);
    result.state        = run.state;
    result.code         = run.code;
    return result;
}

// ---- 以下 helper 来自 #40（tag 定位 clamp）----

// ID3v2 头固定 10 字节，体长度是 4 个 syncsafe 字节。这里只造头，实体另行决定，
// 因为「声明长度」与「实体长度」在坏素材里恰好是两件事（规格 7.2 第 2 条）。
std::vector<char> makeId3Header(size_t bodySize)
{
    std::vector<char> header(10, 0);
    header[0] = 'I';
    header[1] = 'D';
    header[2] = '3';
    header[3] = 3; // ID3v2.3
    header[4] = 0;
    header[5] = 0; // 无标志位
    header[6] = static_cast<char>((bodySize >> 21) & 0x7F);
    header[7] = static_cast<char>((bodySize >> 14) & 0x7F);
    header[8] = static_cast<char>((bodySize >> 7) & 0x7F);
    header[9] = static_cast<char>(bodySize & 0x7F);
    return header;
}

// 头 + bodyBytes 个 0 字节 + 原音频。bodyBytes 与头里声明的长度可以不一致，
// 那正是「声明与文件长度矛盾」这一类的造法。
std::vector<char> wrapWithId3(const std::vector<char> &audio, const std::vector<char> &header,
                              size_t bodyBytes)
{
    std::vector<char> wrapped = header;
    wrapped.insert(wrapped.end(), bodyBytes, '\0');
    wrapped.insert(wrapped.end(), audio.begin(), audio.end());
    return wrapped;
}

// 提取器给出的 payload（getMetaData）。规格 7.3 M6：跳过 tag 不得改动它。
std::vector<char> extractorPayload(const std::string &path, const char *extension)
{
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return std::vector<char>();
    }
    std::unique_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(&source, extension, true));
    if (!extractor || extractor->initCheck() != sdk_utils::OK) {
        return std::vector<char>();
    }
    AudioBuffer::AudioBufferPtr payload = extractor->getMetaData();
    if (!payload || payload->size() == 0) {
        return std::vector<char>();
    }
    return std::vector<char>(payload->data(), payload->data() + payload->size());
}



// ---- 以下 helper 来自 #41（矩阵收口）----

// 改 wav fmt 的声道数并同步 byteRate / blockAlign，让头自身保持自洽。
// 规格 9 第 4 条：PCM 头是唯一自述，改完没有任何第二来源可以比对。
bool patchWavChannels(std::vector<char> &data, uint16_t channels)
{
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || channels == 0) {
        return false;
    }
    size_t offset = 12;
    while (offset + 8 <= data.size()) {
        const uint32_t chunkSize = readU32LE(data, offset + 4);
        if (std::memcmp(data.data() + offset, "fmt ", 4) == 0) {
            const size_t body = offset + 8;
            if (body + 16 > data.size()) {
                return false;
            }
            const auto u16 = [&data](size_t at) -> uint16_t {
                return static_cast<uint16_t>(static_cast<unsigned char>(data[at]))
                    | (static_cast<uint16_t>(static_cast<unsigned char>(data[at + 1])) << 8);
            };
            const uint32_t sampleRate     = readU32LE(data, body + 4);
            const uint16_t bits           = u16(body + 14);
            const uint32_t bytesPerSample = bits / 8;
            if (sampleRate == 0 || bytesPerSample == 0) {
                return false;
            }
            data[body + 2] = static_cast<char>(channels & 0xff);
            data[body + 3] = static_cast<char>((channels >> 8) & 0xff);
            writeU32LE(data, body + 8, sampleRate * channels * bytesPerSample);
            const uint16_t blockAlign = static_cast<uint16_t>(channels * bytesPerSample);
            data[body + 12] = static_cast<char>(blockAlign & 0xff);
            data[body + 13] = static_cast<char>((blockAlign >> 8) & 0xff);
            return true;
        }
        offset += 8 + static_cast<size_t>(chunkSize) + (chunkSize & 1);
    }
    return false;
}

// 改 mp4 stsd 里 mp4a sample entry 的容器层 spec 字段（+24 channelCount、+32
// sampleRate 16.16）。规格 9 第 5 条：这些字段根本不进决策链，提取器读 codecpar、
// 解码器读码流 ASC。m4a 是四个 ffmpeg 格式的代表，共用同一条路径。
bool patchMp4StsdSpec(std::vector<char> &data, uint32_t sampleRate, uint16_t channels)
{
    for (size_t i = 4; i + 36 <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, "mp4a", 4) != 0) {
            continue;
        }
        const size_t entry = i - 4;
        const uint32_t size = readU32BE(data, entry);
        if (size < 36 || entry + size > data.size()) {
            continue;
        }
        data[entry + 24] = static_cast<char>((channels >> 8) & 0xff);
        data[entry + 25] = static_cast<char>(channels & 0xff);
        writeU32BE(data, entry + 32, sampleRate << 16);
        return true;
    }
    return false;
}

} // namespace

TEST_SUITE("BadInput")
{
    TEST_CASE("corrupted mid-stream reports DataCorrupted after audio was heard")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("像我这样的人-毛不易.128.mp3")) {
            return;
        }
        TempFileSet tmp("codec");
        std::vector<char> data = readWholeFile(fixture.mediaPath("像我这样的人-毛不易.128.mp3"));
        REQUIRE(!data.empty());
        zeroRange(data, data.size() * 30 / 100, 4096);
        std::string badPath = tmp.make(".mp3");
        REQUIRE(writeWholeFile(badPath, data));

        FileSource source(badPath.c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        std::shared_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".mp3", true));
        REQUIRE(extractor != nullptr);

        AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
        AudioStreamDecoder decoder(&ring, makeDevSpec());
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        DrainResult result = drainUntilSettled(decoder, ring, 20000);
        decoder.stop();

        CHECK(result.settled);
        CHECK(result.endState == StreamDecoderState::ERROR);
        // 用户先听到了前面那段，才在坏处停下（B4：量的是听得到的字节，不是「拒绝」）
        CHECK(result.bytes > 0);
        CHECK(decoder.errorCode() == sdk::ErrorCode::DataCorrupted);
        CHECK(!decoder.errorDetail().empty());
    }

    TEST_CASE("player reports decode error instead of stalling silently")
    {
        RealMediaFixture fixture;
        // #37 之后单包坏帧是「跳过继续」，错误要等 EOS 才汇总，所以素材必须短到能在
        // 用例超时内播完：3.7 s 的 ogg，坏页在 50% 处，播到 2.7 s 就 EOS 并汇总。
        if (!fixture.exists("Ringtones-耳聆网.ogg")) {
            return;
        }
        TempFileSet tmp("player");
        std::string dir = tmp.makeDir();
        REQUIRE(!dir.empty());
        std::vector<char> data = readWholeFile(fixture.mediaPath("Ringtones-耳聆网.ogg"));
        REQUIRE(!data.empty());
        zeroRange(data, data.size() / 2, 100);
        // 单曲目录：autoSkip 因「没有下一首」关闭，停下来的位置是确定的
        REQUIRE(writeWholeFile(dir + "/broken.ogg", data));

        RecordingPlayerListener listener;
        std::string logDir = "./log";
        {
            sdk::MusicPlayer player(&listener, logDir);
            player.addMusicDir(dir);
            REQUIRE(waitFor([&player]() { return player.getMusicCount() == 1; }, 20000));

            player.play();
            if (!waitFor(
                    [&player]() {
                        return player.state() == sdk::MusicPlayerState::PlayingState;
                    },
                    3000)) {
                // 无声卡环境（CI 容器）：拿不到播放状态就没法观测播放路径，跳过
                std::printf("[SKIP] audio device did not start\n");
                return;
            }

            REQUIRE(waitFor([&listener]() { return listener.errorCount() > 0; }, 20000));

            std::vector<RecordingPlayerListener::Report> reports = listener.snapshot();
            REQUIRE(!reports.empty());
            CHECK(reports.back().code == sdk::ErrorCode::DataCorrupted);
            CHECK(reports.back().index == 0);
            // 坏处之后不再显示 Playing：静默卡死是缺陷（规格 8.2）
            CHECK(waitFor(
                [&player]() {
                    return player.state() != sdk::MusicPlayerState::PlayingState;
                },
                2000));
            player.stop();
        }
    }

    TEST_CASE("batch scan reports a summary for rejected files")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }
        TempFileSet tmp("batch");
        std::string dir = tmp.makeDir();
        REQUIRE(!dir.empty());

        std::vector<char> good = readWholeFile(fixture.mediaPath("music.wav"));
        REQUIRE(!good.empty());
        REQUIRE(writeWholeFile(dir + "/a_good.wav", good));
        std::vector<char> broken = good;
        zeroRange(broken, 0, 512);
        REQUIRE(writeWholeFile(dir + "/b_broken.wav", broken));

        RecordingPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);

        player.addMusicDir(dir);
        auto hasSummary = [&listener]() {
            for (const RecordingPlayerListener::Report &report : listener.snapshot()) {
                if (report.index < 0
                    && report.detail.find("cannot be played") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        REQUIRE(waitFor(hasSummary, 20000));

        std::vector<RecordingPlayerListener::Report> reports = listener.snapshot();
        bool foundSummary                                    = false;
        for (const RecordingPlayerListener::Report &report : reports) {
            if (report.index < 0 && report.detail.find("cannot be played") != std::string::npos) {
                foundSummary = true;
            }
        }
        // 被拒文件不进播放列表，永远不会成为「当前曲目」——只能靠批次末尾的汇总让它可见
        CHECK(foundSummary);
        CHECK(player.state() == sdk::MusicPlayerState::StoppedState);
    }

    TEST_CASE("auto skip does not revisit the current track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }
        RecordingPlayListCallback callback;
        auto playlist = makePlayList(callback, { fixture.mediaPath("music.wav") });
        REQUIRE(playlist->getMusicCount() == 1);

        // 只有一曲：绕回自己就是「同一曲重试第二次」，应当返回 false 让调用方停在坏处
        CHECK(playlist->skipToNextPlayable() == false);
        CHECK(callback.lastCurrentIndex == 0);
    }

    TEST_CASE("auto skip skips tracks that already failed in this session")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }
        RecordingPlayListCallback callback;
        auto playlist = makePlayList(callback, { fixture.mediaPath("music.wav"),
                                                 fixture.mediaPath("music.wav"),
                                                 fixture.mediaPath("music.wav") });
        REQUIRE(playlist->getMusicCount() == 3);

        playlist->markTrackFailed(0);
        playlist->markTrackFailed(1);
        CHECK(playlist->skipToNextPlayable() == true);
        CHECK(callback.lastCurrentIndex == 2);

        playlist->markTrackFailed(2);
        CHECK(playlist->skipToNextPlayable() == false);

        playlist->clearFailedTracks();
        CHECK(playlist->isTrackFailed(2) == false);
    }

    TEST_CASE("truncated and corrupted errors read differently")
    {
        const sdk::ErrorCodeInfo &truncated = sdk::GetErrorInfo(sdk::ErrorCode::DataTruncated);
        const sdk::ErrorCodeInfo &corrupted = sdk::GetErrorInfo(sdk::ErrorCode::DataCorrupted);

        CHECK(truncated.recoverable == true);
        CHECK(corrupted.recoverable == true);
        CHECK(truncated.action == sdk::ErrorAction::SkipTrack);
        CHECK(corrupted.action == sdk::ErrorAction::SkipTrack);
        // 不同错误码不共用一句：截断是「可能不完整」，坏帧是「已损坏」
        CHECK(std::string(truncated.message) != std::string(corrupted.message));
        CHECK(truncated.message != nullptr);
        CHECK(corrupted.message != nullptr);
    }

    // ---- 以下用例来自 #35（进程安全票）----

    TEST_CASE("flac streaminfo with zero sample rate is rejected without crash")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }
        TempFileSet tmp("flac_zero_sr");
        std::vector<char> data = readWholeFile(fixture.mediaPath("小镇姑娘-陶喆.flac"));
        REQUIRE(!data.empty());
        // 只在 [12,140) 做手术：魔数留着，紧跟其后的必需结构被毁
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".flac");
        REQUIRE(writeWholeFile(badPath, data));

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".flac");
        long long cost                              = elapsedMs(begin);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        // 拒绝时不允许把「sr=0」这种 unusable spec 当成正常结果带出去
        CHECK(outcome.sampleRate == 0);
        CHECK(outcome.durationMs == 0);
    }

    // 规格 10.#2：旧代码用 `uint8_t data[blockSize]`（栈上变长数组）读 STREAMINFO，
    // blockSize 上限 0xFFFFFF = 16MB，超过默认栈就是栈溢出。
    TEST_CASE("flac oversized metadata block does not allocate on the stack")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }
        TempFileSet tmp("flac_big_block");
        std::vector<char> data = readWholeFile(fixture.mediaPath("小镇姑娘-陶喆.flac"));
        REQUIRE(!data.empty());
        // FLAC 布局：[0,4) "fLaC"，[4,8) 第一个元数据块头（1B 标志位 + 3B 长度），
        // 把长度改写成 0xFFFFFF，块类型仍是 STREAMINFO
        data[5] = static_cast<char>(0xFF);
        data[6] = static_cast<char>(0xFF);
        data[7] = static_cast<char>(0xFF);
        std::string badPath = tmp.make(".flac");
        REQUIRE(writeWholeFile(badPath, data));

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".flac");
        long long cost                              = elapsedMs(begin);

        CHECK(cost < 5000);
        // STREAMINFO 恒为 34 字节，声明 16MB 一定是坏头
        CHECK(rejected(outcome));
    }

    // 规格 10.#3 / 5.5 #5：对象长度为 0 时 offset 原地打转，6 秒能写 5.4 万行日志。
    // 红的表现是这条用例不返回（由 ctest 的 TIMEOUT 兜底），绿的表现是秒级返回。
    TEST_CASE("asf object with zero size terminates instead of spinning")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("萨克斯机.asf")) {
            return;
        }
        TempFileSet tmp("asf_zero_obj");
        std::vector<char> data = readWholeFile(fixture.mediaPath("萨克斯机.asf"));
        REQUIRE(!data.empty());
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".asf");
        REQUIRE(writeWholeFile(badPath, data));

        long long logBefore                         = logDirBytes(kLogDir);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".asf");
        long long cost                              = elapsedMs(begin);
        long long logAfter                          = logDirBytes(kLogDir);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        // 一次拒绝最多留几十行日志；1MB 是给并发日志留的余量，仍远小于「每秒 9 万行」
        CHECK(logAfter - logBefore < 1024 * 1024);
    }

    TEST_CASE("wma object with zero size terminates instead of spinning")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("十七岁-陶喆.96.wma")) {
            return;
        }
        TempFileSet tmp("wma_zero_obj");
        std::vector<char> data = readWholeFile(fixture.mediaPath("十七岁-陶喆.96.wma"));
        REQUIRE(!data.empty());
        zeroRange(data, 12, 128);
        std::string badPath = tmp.make(".wma");
        REQUIRE(writeWholeFile(badPath, data));

        long long logBefore                         = logDirBytes(kLogDir);
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ExtractorOutcome outcome                    = openExtractor(badPath, ".wma");
        long long cost                              = elapsedMs(begin);
        long long logAfter                          = logDirBytes(kLogDir);

        CHECK(cost < 5000);
        CHECK(rejected(outcome));
        CHECK(logAfter - logBefore < 1024 * 1024);
    }

    // 规格 10.#4 / 5.5 #3：旧代码从不校验 syncWord，全零头被解析成 sr=96000 ch=0 且 init 成功，
    // 一份垃圾头就这样进了播放列表。
    TEST_CASE("aac header with zero sync word is rejected")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("WavinFlag.aac")) {
            return;
        }
        TempFileSet tmp("aac_zero_sync");
        std::vector<char> data = readWholeFile(fixture.mediaPath("WavinFlag.aac"));
        REQUIRE(!data.empty());
        zeroRange(data, 0, 12);
        std::string badPath = tmp.make(".aac");
        REQUIRE(writeWholeFile(badPath, data));

        ExtractorOutcome outcome = openExtractor(badPath, ".aac");
        CHECK(rejected(outcome));
    }

    TEST_CASE("aac header with zero channel configuration is rejected")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("WavinFlag.aac")) {
            return;
        }
        TempFileSet tmp("aac_zero_ch");
        std::vector<char> data = readWholeFile(fixture.mediaPath("WavinFlag.aac"));
        REQUIRE(!data.empty());
        // 只清 channelConfiguration 的三个位：data[2] bit0 + data[3] bit7..6。
        // syncWord / samplingFrequencyIndex 原样保留，所以「靠同步字侥幸通过」的实现在这里仍然会红。
        clearBits(data, 2, 0x01);
        clearBits(data, 3, 0xC0);
        std::string badPath = tmp.make(".aac");
        REQUIRE(writeWholeFile(badPath, data));

        ExtractorOutcome outcome = openExtractor(badPath, ".aac");
        CHECK(rejected(outcome));
    }

    // ---- 以下用例来自 #38（头坏：拒绝的唯一出口）----

    // 规格 B2 / 10.#14：工厂出口是唯一的拒绝点。init 成功但 spec 空的替身过去会原样放行。
    TEST_CASE("factory rejects registered extractor with empty spec")
    {
        const std::string extension = ".bad_input_empty_spec_probe";
        REQUIRE(ExtractorFactory::registerExtractor(
            extension, [](DataSourceBase *) -> ExtractorHelper * {
                return new FixedSpecProbeExtractor(0, 0, sdk_utils::OK);
            }));

        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(nullptr, extension, false));
        CHECK(extractor == nullptr);
    }

    // 规格 B2：initCheck 失败同样在出口处收成 nullptr，调用方只剩一种判据。
    TEST_CASE("factory rejects registered extractor whose init failed")
    {
        const std::string extension = ".bad_input_init_failed_probe";
        REQUIRE(ExtractorFactory::registerExtractor(
            extension, [](DataSourceBase *) -> ExtractorHelper * {
                return new FixedSpecProbeExtractor(44100, 2, sdk_utils::NO_INIT);
            }));

        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(nullptr, extension, false));
        CHECK(extractor == nullptr);
    }

    // 规格 10.#14 的逃生口：空 spec 是契约违例，只有显式 opt-in 的替身能通过。
    TEST_CASE("factory allows empty spec only for registrations that opt in")
    {
        const std::string extension = ".bad_input_empty_spec_allowed_probe";
        REQUIRE(ExtractorFactory::registerExtractor(
            extension,
            [](DataSourceBase *) -> ExtractorHelper * {
                return new FixedSpecProbeExtractor(0, 0, sdk_utils::OK);
            },
            nullptr, true));

        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(nullptr, extension, false));
        REQUIRE(extractor != nullptr);
        CHECK(extractor->getAudioSpec().sampleRate == 0);
    }

    // 规格 5.4 H4：判定「扩展名是否注册」必须有查询口，不能靠 catch nullptr 再猜。
    TEST_CASE("factory reports which extensions are registered")
    {
        CHECK(ExtractorFactory::isSupportedExtension(".wav"));
        CHECK(ExtractorFactory::isSupportedExtension("WAV"));
        CHECK(ExtractorFactory::isSupportedExtension("flac"));
        CHECK(!ExtractorFactory::isSupportedExtension(".xyz"));
        CHECK(!ExtractorFactory::isSupportedExtension(""));
    }

    // 规格 5.5 扩展名冲突组 / 10.#26：同一份完好 wav，三种扩展名都必须是同一个结论。
    // 今天是自相矛盾的：.xyz 靠魔数救回来，.mp3 却直接判损坏。
    TEST_CASE("same wav opens through wav xyz and mp3 extensions with identical spec")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }
        TempFileSet tmp("extension_conflict");
        std::vector<char> data = readWholeFile(fixture.mediaPath("music.wav"));
        REQUIRE(!data.empty());
        const std::string path = tmp.make(".wav");
        REQUIRE(writeWholeFile(path, data));

        ExtractorOutcome baseline = openExtractor(path, ".wav");
        REQUIRE(baseline.created);
        REQUIRE(baseline.initOk);
        CHECK(baseline.sampleRate > 0);
        CHECK(baseline.numChannel > 0);

        const char *extensions[] = { ".xyz", ".mp3" };
        for (const char *extension : extensions) {
            INFO("extension: " << extension);
            ExtractorOutcome outcome = openExtractor(path, extension);
            CHECK(outcome.created);
            CHECK(outcome.initOk);
            CHECK(outcome.sampleRate == baseline.sampleRate);
            CHECK(outcome.numChannel == baseline.numChannel);
            CHECK(outcome.durationMs == baseline.durationMs);
        }
    }

    // 规格 5.4 H3 / 8.1：错误码按「扩展名是否注册」分，不按「解析失败」分。
    // 注册过的扩展名被工厂拒绝 = 头坏（ExtractorInitFailed），不是「格式不支持」。
    TEST_CASE("rejected registered extension reports extractor init failed")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }
        TempFileSet tmp("err_init_failed");
        std::vector<char> data = readWholeFile(fixture.mediaPath("music.wav"));
        REQUIRE(!data.empty());
        zeroRange(data, 0, 512);
        const std::string path = tmp.make(".wav");
        REQUIRE(writeWholeFile(path, data));

        RecordingPlayListCallback callback;
        auto playlist = makePlayList(callback, { path });

        // 拒绝后不产生任何可播内容：不成为当前曲目，也就没有半截音频漏进 ring（H5）
        CHECK(playlist->getMusicCount() == 0);
        CHECK(callback.currentBufCount == 0);
        REQUIRE(callback.errors.size() == 1);
        CHECK(callback.errors[0].code == sdk::ErrorCode::ExtractorInitFailed);
    }

    // 未注册的扩展名 + 魔数认不出 = 格式不支持，不许说成文件损坏
    TEST_CASE("rejected unregistered extension reports extractor unsupported")
    {
        TempFileSet tmp("err_unsupported");
        const std::string path = tmp.make(".xyz");
        std::vector<char> garbage(32, 0x5A);
        REQUIRE(writeWholeFile(path, garbage));

        RecordingPlayListCallback callback;
        auto playlist = makePlayList(callback, { path });

        CHECK(playlist->getMusicCount() == 0);
        CHECK(callback.currentBufCount == 0);
        REQUIRE(callback.errors.size() == 1);
        CHECK(callback.errors[0].code == sdk::ErrorCode::ExtractorUnsupported);
    }

    // ---- 以下用例来自 #36（截断：能解多少播多少）----
    //
    // 判据一律自校准相对式：expect = r × good，good 是同一次运行内完好文件在这条
    // 流式路径上的实测值，坏文件自报的时长一个都不读（规格 1、3.4、8.3）。

    // 规格 3.4 #4 / T2：wav 的 data 声明长度大于实际可读字节。修好前这一档是
    // initCheck 失败、0 字节（A 类，缺口1）；修好后按 min(声明, 剩余) 读，播 60%。
    TEST_CASE("wav truncated in the middle of audio plays what it can and reports truncation")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        StreamMeasurement goodRun = measureStream(goodPath, ".wav", 20000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        // 完好文件必须静默 EOS，否则下面的「报码」失去意义
        CHECK(goodRun.state == StreamDecoderState::EOS);
        CHECK(goodRun.code == sdk::ErrorCode::Ok);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("wav60");
        std::string badPath = tmp.make(".wav");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 60)));

        StreamMeasurement bad = measureStream(badPath, ".wav", 20000);
        REQUIRE(bad.opened);
        const uint64_t measuredMs = devBytesToMs(bad.bytes);
        CHECK(measuredMs > 0);
        CHECK(withinTruncationBand(measuredMs, goodMs * 6 / 10, goodMs));
        CHECK(bad.state == StreamDecoderState::ERROR);
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
        CHECK(!bad.detail.empty());
    }

    // 同一条自校准判据换一个 r：相邻用例 r 差 0.2（规格 T6），
    // 断言随 r 线性缩放，锁住「期望值锚在 good 上」而不是写死某个时长。
    TEST_CASE("wav truncated at a different ratio scales with the intact file")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        StreamMeasurement goodRun = measureStream(goodPath, ".wav", 20000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("wav40");
        std::string badPath = tmp.make(".wav");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 40)));

        StreamMeasurement bad = measureStream(badPath, ".wav", 20000);
        REQUIRE(bad.opened);
        const uint64_t measuredMs = devBytesToMs(bad.bytes);
        CHECK(measuredMs > 0);
        CHECK(withinTruncationBand(measuredMs, goodMs * 4 / 10, goodMs));
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
    }

    // 票 #36 验收项 2：丢掉尾部 128 字节（0.016%）时「解出时长 ≥ 完好文件的 95%，
    // 且上报 DataTruncated」。5pp 阈值吸收不了这么大的偏差，所以 wav/aiff 必须靠
    // 「声明长度 > 文件剩余」这个**确定事实**报码（规格 3.2 的「能判定短读时」）。
    TEST_CASE("wav missing only its last 128 bytes still reports truncation")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(good.size() > 128);

        StreamMeasurement goodRun = measureStream(goodPath, ".wav", 20000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("wav128");
        std::string badPath = tmp.make(".wav");
        REQUIRE(writeWholeFile(badPath, dropTailBytes(good, 128)));

        StreamMeasurement bad = measureStream(badPath, ".wav", 20000);
        REQUIRE(bad.opened);
        const uint64_t measuredMs = devBytesToMs(bad.bytes);
        // 一帧都没丢：长度只少了 4ms 左右，仍然必须「能解多少播多少」
        CHECK(measuredMs * 100 >= goodMs * 95);
        CHECK(withinTruncationBand(measuredMs, goodMs, goodMs));
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
    }

    // 规格 7.2 表里与第 3 节共用的一行：data 声明 2GB、文件 0.8MB。
    // 修好前这一行按声明分配，进程要么 OOM 要么崩；修好后 clamp 到实际剩余。
    TEST_CASE("wav declaring a giant data chunk does not allocate it")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        TempFileSet tmp("wav_giant");
        std::vector<char> data = readWholeFile(fixture.mediaPath(name));
        REQUIRE(data.size() > 44);

        // 定位 "data" 块的长度字段（RIFF: 12 字节头 + 若干 chunk）
        size_t sizeOffset = 0;
        for (size_t off = 12; off + 8 <= data.size();) {
            uint32_t chunkSize = static_cast<uint32_t>(
                (static_cast<unsigned char>(data[off + 4]))
                | (static_cast<unsigned char>(data[off + 5]) << 8)
                | (static_cast<unsigned char>(data[off + 6]) << 16)
                | (static_cast<unsigned char>(data[off + 7]) << 24));
            if (std::memcmp(&data[off], "data", 4) == 0) {
                sizeOffset = off + 4;
                break;
            }
            off += 8 + static_cast<size_t>(chunkSize) + (chunkSize & 1u);
        }
        REQUIRE(sizeOffset != 0);

        const unsigned char huge[4] = { 0x00, 0x00, 0xF0, 0x7F }; // 2,146,435,072 字节
        const unsigned char riff[4] = { 0xFF, 0xFF, 0xFF, 0x7F };
        for (int i = 0; i < 4; ++i) {
            // RIFF 的 totalSize 也要一起改大：否则解析循环在「chunkSize > remainingSize」
            // 就提前退出了，碰不到「按声明分配」那一行
            data[4 + static_cast<size_t>(i)]          = static_cast<char>(riff[i]);
            data[sizeOffset + static_cast<size_t>(i)] = static_cast<char>(huge[i]);
        }
        std::string badPath = tmp.make(".wav");
        REQUIRE(writeWholeFile(badPath, data));

        StreamMeasurement goodRun = measureStream(fixture.mediaPath(name), ".wav", 20000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        StreamMeasurement bad                       = measureStream(badPath, ".wav", 20000);
        long long cost                              = elapsedMs(begin);

        REQUIRE(bad.opened);
        CHECK(cost < 5000);
        // 真实音频一个字节都没少，全部播出来；报码针对的是「声明」而不是「数据」
        CHECK(withinTruncationBand(devBytesToMs(bad.bytes), goodMs, goodMs));
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
    }

    // 规格 3.4 #5：aiff 与 wav 同一档（缺口1 的另一半）。
    TEST_CASE("aiff truncated in the middle of audio plays what it can and reports truncation")
    {
        RealMediaFixture fixture;
        const char *name = "俱乐部音乐循环-Freesound.aiff";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        StreamMeasurement goodRun = measureStream(goodPath, ".aiff", 60000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        CHECK(goodRun.code == sdk::ErrorCode::Ok);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("aiff60");
        std::string badPath = tmp.make(".aiff");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 60)));

        StreamMeasurement bad = measureStream(badPath, ".aiff", 60000);
        REQUIRE(bad.opened);
        const uint64_t measuredMs = devBytesToMs(bad.bytes);
        CHECK(measuredMs > 0);
        CHECK(withinTruncationBand(measuredMs, goodMs * 6 / 10, goodMs));
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
    }

    // 规格 3.4 #3 / T4：修好前 flac 截到 60% 是静默 Completed（用户看到进度条走到
    // 177 秒就结束、界面还写着 297 秒）。EOS 汇总处比对标称时长是唯一修法（条目 23）。
    TEST_CASE("flac truncated at 60 percent reports truncation instead of silent end")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.flac";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        StreamMeasurement goodRun = measureStream(goodPath, ".flac", 120000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        CHECK(goodRun.state == StreamDecoderState::EOS);
        CHECK(goodRun.code == sdk::ErrorCode::Ok);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("flac60");
        std::string badPath = tmp.make(".flac");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 60)));

        StreamMeasurement bad = measureStream(badPath, ".flac", 120000);
        REQUIRE(bad.opened);
        const uint64_t measuredMs = devBytesToMs(bad.bytes);
        CHECK(measuredMs > 0);
        CHECK(withinTruncationBand(measuredMs, goodMs * 6 / 10, goodMs));
        CHECK(bad.code == sdk::ErrorCode::DataTruncated);
    }

    // 规格 3.2 P0 / 3.4 #1：切在音频之后（ID3v1 尾标）→ measured == good 且完全静默。
    TEST_CASE("mp3 cut inside the trailing tag stays silent")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.128.mp3";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(good.size() > 128);
        // P0 的前提：尾部 128 字节正好是 ID3v1 尾标，即砍掉的不是音频（规格 3.2 末段）。
        // 规格写的是「末 3 字节为 TAG」，那是 ID3v1 的**块首**位置——尾标占最后 128 字节，
        // 末 3 字节是 genre/comment。所以判据落在 size-128 上。
        // 不满足就 SKIP 而不是 FAIL——素材换了也不该让用例变成假红。
        const size_t tagStart = good.size() - 128;
        if (good[tagStart] != 'T' || good[tagStart + 1] != 'A' || good[tagStart + 2] != 'G') {
            std::printf("[SKIP] %s has no 128-byte ID3v1 tail\n", name);
            return;
        }

        StreamMeasurement goodRun = measureStream(goodPath, ".mp3", 60000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("mp3_p0");
        std::string badPath = tmp.make(".mp3");
        REQUIRE(writeWholeFile(badPath, dropTailBytes(good, 128)));

        StreamMeasurement bad = measureStream(badPath, ".mp3", 60000);
        REQUIRE(bad.opened);
        CHECK(withinTruncationBand(devBytesToMs(bad.bytes), goodMs, goodMs));
        // P0 不报错：只有「切在音频中间」（P1）才要求 DataTruncated（规格 3.2）
        CHECK(bad.state == StreamDecoderState::EOS);
        CHECK(bad.code == sdk::ErrorCode::Ok);
    }

    // 规格 3.2 P0 / 3.4 #2：mkv 尾部是非音频区（索引/附件），砍 128 字节同样静默。
    TEST_CASE("mkv cut at the tail stays silent")
    {
        RealMediaFixture fixture;
        const char *name = "music-mkv.mkv";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(good.size() > 128);

        StreamMeasurement goodRun = measureStream(goodPath, ".mkv", 60000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("mkv_p0");
        std::string badPath = tmp.make(".mkv");
        REQUIRE(writeWholeFile(badPath, dropTailBytes(good, 128)));

        StreamMeasurement bad = measureStream(badPath, ".mkv", 60000);
        REQUIRE(bad.opened);
        CHECK(withinTruncationBand(devBytesToMs(bad.bytes), goodMs, goodMs));
        CHECK(bad.state == StreamDecoderState::EOS);
        CHECK(bad.code == sdk::ErrorCode::Ok);
    }

    // 规格 3.4 #11 / 3.2 P2：切在必需结构中间（头之后不足一帧）→ 拒绝，ring 0 字节。
    TEST_CASE("wav cut below one frame is rejected and plays nothing")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        TempFileSet tmp("wav20");
        std::vector<char> good = readWholeFile(fixture.mediaPath(name));
        REQUIRE(good.size() > 20);
        std::vector<char> tiny(good.begin(), good.begin() + 20);
        std::string badPath = tmp.make(".wav");
        REQUIRE(writeWholeFile(badPath, tiny));

        CHECK(rejected(openExtractor(badPath, ".wav")));
        StreamMeasurement bad = measureStream(badPath, ".wav", 5000);
        CHECK(!bad.opened);
        CHECK(bad.bytes == 0);
    }

    // 规格 3.2 P2 / T3：ogg 的截断一律拒绝（第三方 demux 的页链校验，自有代码没有
    // clamp 的着力点，写成已知例外）。ring 累计 0 字节且报错。
    TEST_CASE("ogg truncation is rejected and plays nothing")
    {
        RealMediaFixture fixture;
        const char *name = "Ringtones-耳聆网.ogg";
        if (!fixture.exists(name)) {
            return;
        }
        TempFileSet tmp("ogg60");
        std::vector<char> good = readWholeFile(fixture.mediaPath(name));
        REQUIRE(!good.empty());
        std::string badPath = tmp.make(".ogg");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 60)));

        CHECK(rejected(openExtractor(badPath, ".ogg")));
        StreamMeasurement bad = measureStream(badPath, ".ogg", 10000);
        CHECK(bad.bytes == 0);
        CHECK(bad.state != StreamDecoderState::EOS);
    }

    // 规格 3.4 #10 / 3.2 P2：m4a 的 moov 在尾部，砍 1KB 把它切断 → 拒绝。
    // 素材换成 music-alac.m4a：规格点名的 摇滚乐_Freesound.m4a 其实是 faststart 布局
    // （moov 在前、mdat 在后），砍尾巴砍到的是音频，见 findings-36。
    TEST_CASE("m4a with its trailing moov cut off is rejected")
    {
        RealMediaFixture fixture;
        const char *name = "music-alac.m4a";
        if (!fixture.exists(name)) {
            return;
        }
        TempFileSet tmp("m4a_moov");
        std::vector<char> good = readWholeFile(fixture.mediaPath(name));
        REQUIRE(good.size() > 1024);

        // P2 的前提：末端确实是一个 moov（顶层 box 链走一遍）。不满足就 SKIP。
        bool tailIsMoov = false;
        size_t off      = 0;
        while (off + 8 <= good.size()) {
            uint32_t boxSize = static_cast<uint32_t>(
                (static_cast<unsigned char>(good[off]) << 24)
                | (static_cast<unsigned char>(good[off + 1]) << 16)
                | (static_cast<unsigned char>(good[off + 2]) << 8)
                | static_cast<unsigned char>(good[off + 3]));
            if (boxSize < 8) {
                break;
            }
            tailIsMoov = std::memcmp(&good[off + 4], "moov", 4) == 0
                && off + boxSize == good.size();
            off += boxSize;
        }
        if (!tailIsMoov) {
            std::printf("[SKIP] %s is faststart (moov is not in the tail)\n", name);
            return;
        }

        std::string badPath = tmp.make(".m4a");
        REQUIRE(writeWholeFile(badPath, dropTailBytes(good, 1024)));

        CHECK(rejected(openExtractor(badPath, ".m4a")));
        StreamMeasurement bad = measureStream(badPath, ".m4a", 10000);
        CHECK(bad.bytes == 0);
    }

    // ---- 以下用例来自 #37（坏帧：跳过继续）----
    //
    // 规格 4.1 的判别原则：坏帧之后还有数据，截断之后没有。所以判据只有两条——
    // `measured ≈ good`（证明没有因为坏帧提前结束）与「一条汇总 DataCorrupted，
    // 且不是 DataTruncated」。跳帧数、PCM 内容一概不断言（规格 F3）。

    // 规格 4.3 / 条目 10：libFLAC 只把 LOST_SYNC 交给 error_callback，自行 resync 到下一帧，
    // 所以帧级计数分不清「跳了帧」还是「解了垃圾」——唯一可靠的信号是 STREAMINFO 的 MD5。
    TEST_CASE("flac bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.flac";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".flac", 400, 120000, "flac_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.state == StreamDecoderState::ERROR);
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
        CHECK(!run.detail.empty());
    }

    // 规格 4.3 / 条目 11：单包解不出要跳过并计数，不能整段停。
    TEST_CASE("mp3 bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "像我这样的人-毛不易.128.mp3";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".mp3", 400, 60000, "mp3_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    TEST_CASE("aac bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "WavinFlag.aac";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".aac", 200, 60000, "aac_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    // APE 的包粒度是 73728 样本（≈1.67 s，实测 12 个包解完这首 24.9 s 的曲子），
    // 一个包坏掉就按整包丢弃，代价是 6.7pp——超过 T6 的 5pp 带宽。所以这一档的
    // 「没有提前结束」用「播过了损坏点（50%）并一路到 EOS」来判，而不是 5pp。
    TEST_CASE("ape bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "music-ape.ape";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".ape", 200, 60000, "ape_bf");
        REQUIRE(run.opened);
        // 损坏点在文件 50% 处（≈12.4 s），解出 88% 说明确实跳过了坏包继续播
        CHECK(run.measuredMs * 10 >= run.goodMs * 8);
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    TEST_CASE("m4a bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "摇滚乐_Freesound.m4a";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".m4a", 400, 60000, "m4a_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    TEST_CASE("asf bad frame plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "萨克斯机.asf";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".asf", 400, 60000, "asf_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    // 规格 4.3 / 条目 12：`OV_HOLE(-3)` 是「跳页」，不是 `-1` 的整段失败。
    // 素材只有 3.7 s / 10.6 KB，一页就装走了 ≈1 s 音频（26%），整页 CRC 失败后
    // 这页的音频必然丢掉——5pp 带宽在这种页粒度上不可能成立（见 findings-37）。
    // 这一档判「播过了损坏点（50% 处的页）并继续到结尾」，不判 5pp。
    TEST_CASE("ogg bad page plays to the end and reports corruption")
    {
        RealMediaFixture fixture;
        const char *name = "Ringtones-耳聆网.ogg";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".ogg", 100, 60000, "ogg_bf");
        REQUIRE(run.opened);
        // 损坏页在 50% 处（≈1.85 s）；解出 73% 说明跳到下一页后一直播到结尾
        CHECK(run.measuredMs * 10 >= run.goodMs * 6);
        CHECK(run.code == sdk::ErrorCode::DataCorrupted);
    }

    // 规格 4.3 护栏：连续失败 ≥32 帧就放弃，不在垃圾区里空转。
    // 直接驱动 AudioDecode 才判得准：走 mp3 那种带 parser 的编解码器时，parser 自己会把
    // 找不到同步字的垃圾静默吃掉，根本形不成「单包解不出」；而逐包路径上「32 次后放弃」
    // 与「无限跳下去」在播放时长上分不出来（两者都停在坏处附近）。
    // 这里用 wavpack：它没有 parser，包会直接丢给解码器，垃圾包稳定返回 AVERROR_INVALIDDATA。
    TEST_CASE("ffmpeg path gives up after 32 consecutive undecodable packets")
    {
        SkipCountingCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_WAVPACK, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);

        std::vector<char> garbage(1024, static_cast<char>(0xFF));
        std::vector<AudioBuffer::AudioBufferPtr> packets;
        for (int i = 0; i < 40; ++i) {
            AudioBuffer::AudioBufferPtr buf = std::make_shared<AudioBuffer>(garbage.size());
            std::memcpy(buf->data(), garbage.data(), garbage.size());
            packets.push_back(buf);
        }

        int ret = decode.decodePackets(packets);

        // 放弃：40 个包只吃到阈值就停，没把整段垃圾扫完
        CHECK(ret < 0);
        CHECK(decode.skippedPacketCount() == 32);
        // 跳过通知按发生顺序发，第 32 次当场放弃，所以只发到 31 次
        CHECK(callback.skips == 31);
    }

    TEST_CASE("a long run of undecodable packets is abandoned instead of scanned")
    {
        RealMediaFixture fixture;
        const char *name = "摇滚乐_Freesound.m4a";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        StreamMeasurement goodRun = measureStream(goodPath, ".m4a", 60000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("m4a_garbage");
        std::vector<char> broken = good;
        zeroRange(broken, broken.size() / 2, broken.size() * 40 / 100);
        std::string badPath = tmp.make(".m4a");
        REQUIRE(writeWholeFile(badPath, broken));

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        StreamMeasurement bad                       = measureStream(badPath, ".m4a", 60000);
        long long cost                              = elapsedMs(begin);

        REQUIRE(bad.opened);
        CHECK(cost < 20000);
        // 停在垃圾区里（≈50%），而不是一路扫到文件尾（那样会是 ≈90%）
        CHECK(devBytesToMs(bad.bytes) * 10 <= goodMs * 7);
        CHECK(bad.code == sdk::ErrorCode::DataCorrupted);
    }

    // 规格 F4 / 第 9 节：wav、aiff、mkv 的坏帧**不可检测**——坏字节直接变成错误样本，
    // 长度不变、内容变了，没有任何帧级校验。这三条是反向用例：将来谁加了假信号，它们就红。
    TEST_CASE("wav bad frame stays silent because it is undetectable")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".wav", 400, 20000, "wav_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.state == StreamDecoderState::EOS);
        CHECK(run.code == sdk::ErrorCode::Ok);
    }

    TEST_CASE("aiff bad frame stays silent because it is undetectable")
    {
        RealMediaFixture fixture;
        const char *name = "俱乐部音乐循环-Freesound.aiff";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".aiff", 400, 60000, "aiff_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.state == StreamDecoderState::EOS);
        CHECK(run.code == sdk::ErrorCode::Ok);
    }

    TEST_CASE("mkv bad frame stays silent because it is undetectable")
    {
        RealMediaFixture fixture;
        const char *name = "music-mkv.mkv";
        if (!fixture.exists(name)) {
            return;
        }
        BadFrameMeasurement run
            = measureMidFrameCorruption(fixture.mediaPath(name), ".mkv", 400, 60000, "mkv_bf");
        REQUIRE(run.opened);
        CHECK(withinTruncationBand(run.measuredMs, run.goodMs, run.goodMs));
        CHECK(run.state == StreamDecoderState::EOS);
        CHECK(run.code == sdk::ErrorCode::Ok);
    }

    // ---- 规格 6.2 / 条目 16、17：时长来源前移 ----
    //
    // 判据是差分（规格 S2）：篡改那个声明字段，同一份文件改前改后两次运行的
    // durationMs 与解出字节必须逐位不变。零容差——比的是同一份文件的两次运行，
    // 不存在量化误差，所以这里没有「差多少算撒谎」的阈值。

    TEST_CASE("m4a duration follows the packet timeline instead of the declared field")
    {
        RealMediaFixture fixture;
        const char *name = "摇滚乐_Freesound.m4a";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        std::vector<char> lie = good;
        REQUIRE(patchMp4Durations(lie));
        TempFileSet tmp("m4a_dur");
        std::string liePath = tmp.make(".m4a");
        REQUIRE(writeWholeFile(liePath, lie));

        DurationLieMeasurement before = measureDuration(goodPath, ".m4a", 120000);
        DurationLieMeasurement after  = measureDuration(liePath, ".m4a", 120000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.decodedBytes > 0);

        // 改的是声明，不是数据：报出的时长与解出的字节都逐位不变。
        CHECK(after.declaredMs == before.declaredMs);
        CHECK(after.decodedBytes == before.decodedBytes);
        CHECK(after.code == before.code);
    }

    TEST_CASE("asf duration follows the packet timeline instead of the declared field")
    {
        RealMediaFixture fixture;
        const char *name = "萨克斯机.asf";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        std::vector<char> lie = good;
        REQUIRE(patchAsfPlayDuration(lie));
        TempFileSet tmp("asf_dur");
        std::string liePath = tmp.make(".asf");
        REQUIRE(writeWholeFile(liePath, lie));

        DurationLieMeasurement before = measureDuration(goodPath, ".asf", 120000);
        DurationLieMeasurement after  = measureDuration(liePath, ".asf", 120000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.decodedBytes > 0);

        CHECK(after.declaredMs == before.declaredMs);
        CHECK(after.decodedBytes == before.decodedBytes);
        CHECK(after.code == before.code);
    }

    // APE 没有可用的差分用例：ffmpeg 4.4.4 的 ape 解复用器给的 pkt->duration 恒为 0，
    // 连包内的 4 字节帧头都由 APE 头的 blocksperframe / finalframeblocks 重写
    //（实测：改 blocksperframe ×2 → 包内帧头从 0x12000 变成 0x24000）。
    // 也就是说这条路径上没有任何独立于声明字段的时间轴，逐包累加无从下手。
    // 提取器仍按 S1 写了累加 + 兜底日志，但 APE 的时长撒谎因此**仍未吸收**。
    // 证据与复现命令见 build/_agent/badinput/findings-39.md。

    // 规格 6.2 表里 flac 是「半可判」：total_samples 是唯一来源，init 期拿不到第二份，
    // 所以这里只锁「改它不影响解出多少」。EOS 汇总按 5pp 规则报 DataTruncated——
    // 那是 S4 记录在案的已知误报（「少了尾巴」与「字段写大」在数据上无法区分）。
    TEST_CASE("flac total samples lie does not change how much audio is decoded")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.flac";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        std::vector<char> lie = good;
        REQUIRE(patchFlacTotalSamples(lie));
        TempFileSet tmp("flac_dur");
        std::string liePath = tmp.make(".flac");
        REQUIRE(writeWholeFile(liePath, lie));

        DurationLieMeasurement before = measureDuration(goodPath, ".flac", 240000);
        DurationLieMeasurement after  = measureDuration(liePath, ".flac", 240000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.decodedBytes > 0);

        CHECK(after.decodedBytes == before.decodedBytes);
        CHECK(before.code == sdk::ErrorCode::Ok);
        CHECK(after.code == sdk::ErrorCode::DataTruncated);
    }

    // 规格 6.2 S3：采样率/声道以解码首帧实测为准，且必须**无条件**覆盖。
    // 声明值恰好等于 device spec 时，惰性重采样器曾整块被跳过（:100 的 `!=`），
    // 首帧的实测值就永远覆盖不上去——于是 44100 的码流被当成 48000 播，快 8.8%。
    TEST_CASE("decoder takes the first frame spec even when the extractor matches the device")
    {
        RealMediaFixture fixture;
        const char *name = "Ringtones-耳聆网.ogg";
        if (!fixture.exists(name)) {
            return;
        }
        std::vector<char> data = readWholeFile(fixture.mediaPath(name));
        REQUIRE(!data.empty());
        TempFileSet tmp("first_frame_spec");
        std::string path = tmp.make(".ogg");
        REQUIRE(writeWholeFile(path, data));

        FileSource source(path.c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        std::shared_ptr<ExtractorHelper> inner(
            ExtractorFactory::createExtractor(&source, ".ogg", true));
        REQUIRE(inner != nullptr);
        REQUIRE(inner->initCheck() == sdk_utils::OK);

        const AudioSpec honest = inner->getAudioSpec();
        if (honest.sampleRate != 44100 || honest.numChannel != 2) {
            std::printf("[SKIP] %s is not 44100/2ch anymore\n", name);
            return;
        }

        // device spec / 谎报值都取 48000/2ch/S16：声明恰好等于设备，正是惰性门被关上的
        // 那个条件（operator== 只看 sampleRate / format / numChannel）。真实码流是 44100。
        AudioSpec dev = makeDevSpec();
        dev.sampleRate = 48000;

        StreamMeasurement reference = measureStreamWithExtractor(inner, dev, 30000);
        REQUIRE(reference.opened);
        REQUIRE(reference.bytes > 0);

        std::shared_ptr<ExtractorHelper> liar(new LyingSpecExtractor(inner, dev));
        StreamMeasurement actual = measureStreamWithExtractor(liar, dev, 30000);
        REQUIRE(actual.opened);

        // 首帧覆盖后两条路径的重采样参数相同（44100 → 48000），输出同量。
        // 修好前这里短 8.8%（44100 的样本被当 48000 直接写出）。
        CHECK(actual.bytes == reference.bytes);
    }

    // 规格 6.3 #8 / 第 9 节：PCM 类的采样率撒谎**不可检测**——头是唯一自述，
    // 改头之后 declared 与 decoded 依旧自洽，用户听到的是 2× 慢放降调。
    // 这条不给「应该检测出来」的期望值：将来谁给 PCM 加一个假检测，它就会红。
    TEST_CASE("wav sample rate lie stays undetectable")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath   = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());

        std::vector<char> lie = good;
        REQUIRE(patchWavSampleRate(lie, 8000));
        TempFileSet tmp("wav_sr_lie");
        std::string liePath = tmp.make(".wav");
        REQUIRE(writeWholeFile(liePath, lie));

        DurationLieMeasurement before = measureDuration(goodPath, ".wav", 120000);
        DurationLieMeasurement after  = measureDuration(liePath, ".wav", 120000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.decodedBytes > 0);

        // 声明跟着头一起变、实测跟着头一起变，两者仍然一致 → 没有任何可判定信号。
        CHECK(after.code == sdk::ErrorCode::Ok);
        CHECK(withinTruncationBand(after.decodedMs, after.declaredMs, after.declaredMs));
        // 同一个文件被当成半速播放：听起来长了一倍（不是「检测出来」，是慢放）。
        CHECK(after.decodedMs + after.decodedMs / 10 >= before.decodedMs * 2);
    }

    // ---- 以下用例来自 #40（tag 定位 clamp）----

    // 规格 7.2 第 2 条 / 7.3 M2：真正坏事的不是「超大」，是「声明与文件长度矛盾」。
    // 旧实现按声明值推进 offset、越过文件末尾后 Resync 失败，整首解出 0 字节。
    TEST_CASE("mp3 tag declaring more than the file holds still plays the whole audio")
    {
        RealMediaFixture fixture;
        const char *name = "小镇姑娘-陶喆.128.mp3";
        if (!fixture.exists(name)) {
            return;
        }
        const std::string goodPath = fixture.mediaPath(name);
        std::vector<char> good     = readWholeFile(goodPath);
        REQUIRE(good.size() > 4096);

        // 头声明 0x0FFFFFFF（≈256MB）的 tag 体，实体 0 字节：音频紧跟在 10 字节头之后，
        // 与实测素材「声明 200MB、文件 4.7MB」同一种矛盾。
        TempFileSet tmp("mp3_huge_id3");
        std::vector<char> wrapped
            = wrapWithId3(good, makeId3Header((static_cast<size_t>(1) << 28) - 1), 0);
        std::string taggedPath = tmp.make(".mp3");
        REQUIRE(writeWholeFile(taggedPath, wrapped));

        ExtractorOutcome baseline = openExtractor(goodPath, ".mp3");
        ExtractorOutcome tagged   = openExtractor(taggedPath, ".mp3");
        REQUIRE(!rejected(baseline));
        CHECK(!rejected(tagged));
        // 判据零容差：同一段音频的两种包装，逐项相同（7.4 #2）
        CHECK(tagged.sampleRate == baseline.sampleRate);
        CHECK(tagged.numChannel == baseline.numChannel);
        CHECK(tagged.durationMs == baseline.durationMs);
        CHECK(tagged.durationMs > 0);

        // M6：payload 从音频首帧开始，必须逐字节相同
        std::vector<char> baselinePayload = extractorPayload(goodPath, ".mp3");
        std::vector<char> taggedPayload   = extractorPayload(taggedPath, ".mp3");
        REQUIRE(!baselinePayload.empty());
        CHECK(taggedPayload == baselinePayload);

        StreamMeasurement before = measureStream(goodPath, ".mp3", 60000);
        StreamMeasurement after  = measureStream(taggedPath, ".mp3", 60000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.bytes > 0);
        CHECK(after.bytes == before.bytes);
        CHECK(after.code == sdk::ErrorCode::Ok);
    }

    // 规格 7.2 第 3 条 / 7.3 M3：countAdtsFrames 跳 ID3，init 却从 offset 0 读 ADTS 头，
    // 200 字节前缀就够把一份合法文件读成 sr=0 ch=4 的「垃圾」。
    TEST_CASE("aac with an id3 prefix reports the same spec and duration as without it")
    {
        RealMediaFixture fixture;
        const char *name = "WavinFlag.aac";
        if (!fixture.exists(name)) {
            return;
        }
        const std::string goodPath = fixture.mediaPath(name);
        std::vector<char> good     = readWholeFile(goodPath);
        REQUIRE(good.size() > 1024);
        // 素材本身不能已经带 ID3，否则「有前缀 / 无前缀」这一对造不出来
        REQUIRE(std::memcmp(good.data(), "ID3", 3) != 0);

        // 规格 7.4 #3 的实测尺寸：真实 200 字节标签 = 10 字节头 + 190 字节体
        const size_t kBodyBytes    = 190;
        TempFileSet tmp("aac_id3_prefix");
        std::vector<char> wrapped = wrapWithId3(good, makeId3Header(kBodyBytes), kBodyBytes);
        std::string taggedPath    = tmp.make(".aac");
        REQUIRE(writeWholeFile(taggedPath, wrapped));

        ExtractorOutcome baseline = openExtractor(goodPath, ".aac");
        ExtractorOutcome tagged   = openExtractor(taggedPath, ".aac");
        REQUIRE(!rejected(baseline));
        CHECK(!rejected(tagged));
        CHECK(tagged.sampleRate == baseline.sampleRate);
        CHECK(tagged.numChannel == baseline.numChannel);
        CHECK(tagged.durationMs == baseline.durationMs);
        CHECK(tagged.durationMs > 0);

        // aac 的 payload 是整文件视图（标签也在里面，规格 7.1），音频字节必须原样不动
        std::vector<char> baselinePayload = extractorPayload(goodPath, ".aac");
        std::vector<char> taggedPayload   = extractorPayload(taggedPath, ".aac");
        // 规格 7.3 M6：getMetaData() 是音频 payload 的整块视图，tag 不进 payload。
        // 旧断言把「payload 里混着 tag」这个缺陷现状当成期望，与规格冲突，
        // 而且会让解码器把前缀当坏帧、EOS 误报 DataCorrupted。
        REQUIRE(baselinePayload.size() == good.size());
        CHECK(taggedPayload == baselinePayload);

        StreamMeasurement before = measureStream(goodPath, ".aac", 60000);
        StreamMeasurement after  = measureStream(taggedPath, ".aac", 60000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.bytes > 0);
        CHECK(after.bytes == before.bytes);
        CHECK(after.code == sdk::ErrorCode::Ok);
    }

    // 规格 10.#20 的后半句：被拒文件必须有汇总（#34 的链路），但 tag 相关路径不许
    // 误入这份汇总——带 tag 的文件是能播的文件（7.3 M6），被算进「无法播放」就是错的。
    TEST_CASE("batch scan does not count tag-wrapped files as rejected")
    {
        RealMediaFixture fixture;
        const char *wavName = "music.wav";
        const char *mp3Name = "小镇姑娘-陶喆.128.mp3";
        const char *aacName = "WavinFlag.aac";
        if (!fixture.exists(wavName) || !fixture.exists(mp3Name) || !fixture.exists(aacName)) {
            return;
        }
        TempFileSet tmp("batch_tag");
        std::string dir = tmp.makeDir();
        REQUIRE(!dir.empty());

        std::vector<char> wav = readWholeFile(fixture.mediaPath(wavName));
        REQUIRE(!wav.empty());
        REQUIRE(writeWholeFile(dir + "/a_good.wav", wav));

        std::vector<char> mp3 = readWholeFile(fixture.mediaPath(mp3Name));
        REQUIRE(!mp3.empty());
        REQUIRE(writeWholeFile(dir + "/b_mp3_huge_tag.mp3",
                               wrapWithId3(mp3, makeId3Header((static_cast<size_t>(1) << 28) - 1),
                                           0)));

        std::vector<char> aac = readWholeFile(fixture.mediaPath(aacName));
        REQUIRE(!aac.empty());
        REQUIRE(writeWholeFile(dir + "/c_aac_id3.aac", wrapWithId3(aac, makeId3Header(190), 190)));

        std::vector<char> broken = wav;
        zeroRange(broken, 0, 512);
        REQUIRE(writeWholeFile(dir + "/d_broken.wav", broken));

        RecordingPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        player.addMusicDir(dir);

        auto summaryDetail = [&listener]() {
            for (const RecordingPlayerListener::Report &report : listener.snapshot()) {
                if (report.index < 0
                    && report.detail.find("cannot be played") != std::string::npos) {
                    return report.detail;
                }
            }
            return std::string();
        };
        REQUIRE(waitFor([&summaryDetail]() { return !summaryDetail().empty(); }, 20000));

        // 4 个文件里只有 d_broken.wav 该被拒：带 tag 的两个不是「无法播放」
        CHECK(summaryDetail() == "1 file cannot be played");
        for (const RecordingPlayerListener::Report &report : listener.snapshot()) {
            CHECK(report.path.find("b_mp3_huge_tag") == std::string::npos);
            CHECK(report.path.find("c_aac_id3") == std::string::npos);
        }
        player.stop();
    }

    // ---- 以下用例来自 #41（矩阵收口）----

    // 规格 9 第 3 条：裸 mp3 流没有「必需头部」可坏，扫到同步字就能播。
    // 把开头清零不会触发任何拒绝或错误码——将来谁给 mp3 加假头校验，这条就会红。
    TEST_CASE("mp3 zeroed head stays undetectable because there is no required header")
    {
        RealMediaFixture fixture;
        const char *name = "像我这样的人-毛不易.128.mp3";
        if (!fixture.exists(name)) {
            return;
        }
        std::vector<char> good = readWholeFile(fixture.mediaPath(name));
        REQUIRE(good.size() > 1024);
        std::vector<char> broken = good;
        zeroRange(broken, 0, 128);
        TempFileSet tmp("mp3_head_zero");
        std::string badPath = tmp.make(".mp3");
        REQUIRE(writeWholeFile(badPath, broken));

        ExtractorOutcome outcome = openExtractor(badPath, ".mp3");
        REQUIRE(!rejected(outcome));
        CHECK(outcome.sampleRate > 0);
        CHECK(outcome.numChannel > 0);
        StreamMeasurement run = measureStream(badPath, ".mp3", 60000);
        REQUIRE(run.opened);
        CHECK(run.bytes > 0);
        CHECK(run.code == sdk::ErrorCode::Ok);
    }

    // 规格 9 第 4 条：PCM 头是唯一自述，声道数改了之后声明与实测一起变，
    // 仍然自洽，没有任何第二来源可比。锁的是「不许伪造检测」。
    TEST_CASE("wav channel count lie stays undetectable")
    {
        RealMediaFixture fixture;
        const char *name = "music.wav";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath = fixture.mediaPath(name);
        std::vector<char> lie = readWholeFile(goodPath);
        REQUIRE(!lie.empty());
        REQUIRE(patchWavChannels(lie, 2));
        TempFileSet tmp("wav_ch_lie");
        std::string liePath = tmp.make(".wav");
        REQUIRE(writeWholeFile(liePath, lie));

        DurationLieMeasurement before = measureDuration(goodPath, ".wav", 120000);
        DurationLieMeasurement after  = measureDuration(liePath, ".wav", 120000);
        REQUIRE(before.opened);
        REQUIRE(after.opened);
        REQUIRE(before.decodedMs > 0);
        CHECK(after.code == sdk::ErrorCode::Ok);
        CHECK(withinTruncationBand(after.decodedMs, after.declaredMs, after.declaredMs));
        // 半长来自「声明与数据一起被改」，不是检测到了什么。
        CHECK(after.declaredMs < before.declaredMs);
    }

    // 规格 9 第 5 条：ffmpeg 格式的容器层 spec 字段根本不进决策链。
    // 改 stsd 的 sr/ch，spec 与 payload 必须逐项无感。
    TEST_CASE("m4a container spec fields never reach spec or payload")
    {
        RealMediaFixture fixture;
        const char *name = "摇滚乐_Freesound.m4a";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(!good.empty());
        std::vector<char> lie = good;
        REQUIRE(patchMp4StsdSpec(lie, 48000, 1));
        TempFileSet tmp("m4a_stsd_lie");
        std::string liePath = tmp.make(".m4a");
        REQUIRE(writeWholeFile(liePath, lie));

        ExtractorOutcome before = openExtractor(goodPath, ".m4a");
        ExtractorOutcome after  = openExtractor(liePath, ".m4a");
        REQUIRE(!rejected(before));
        CHECK(!rejected(after));
        CHECK(after.sampleRate == before.sampleRate);
        CHECK(after.numChannel == before.numChannel);
        CHECK(after.durationMs == before.durationMs);
        CHECK(extractorPayload(liePath, ".m4a") == extractorPayload(goodPath, ".m4a"));
    }

    // 规格 9 第 1 条：D 类截断运行期报不出来（标称时长跟着文件一起缩水），
    // 但判据仍然成立——measured ≈ r × good，只是不能断言错误码。
    TEST_CASE("aac truncation stays silent while the ratio judgement still holds")
    {
        RealMediaFixture fixture;
        const char *name = "WavinFlag.aac";
        if (!fixture.exists(name)) {
            return;
        }
        std::string goodPath = fixture.mediaPath(name);
        std::vector<char> good = readWholeFile(goodPath);
        REQUIRE(good.size() > 4096);
        StreamMeasurement goodRun = measureStream(goodPath, ".aac", 60000);
        REQUIRE(goodRun.opened);
        REQUIRE(goodRun.bytes > 0);
        const uint64_t goodMs = devBytesToMs(goodRun.bytes);
        REQUIRE(goodMs > 0);

        TempFileSet tmp("aac60");
        std::string badPath = tmp.make(".aac");
        REQUIRE(writeWholeFile(badPath, keepPercent(good, 60)));
        StreamMeasurement bad = measureStream(badPath, ".aac", 60000);
        REQUIRE(bad.opened);
        CHECK(withinTruncationBand(devBytesToMs(bad.bytes), goodMs * 6 / 10, goodMs));
        CHECK(bad.code == sdk::ErrorCode::Ok);
    }

    // 规格基石 B2：拒绝的唯一出口是 createExtractor() == nullptr。
    // 逐格式把必需头部清零，断言没有任何一个格式从「init 失败但对象还在」这条
    // 半吊子路径漏出去（#38 收口前工厂只 LOGW，会带出 spec 为空的对象）。
    TEST_CASE("every rejected format exits through createExtractor returning null")
    {
        struct Item {
            const char *name;
            const char *extension;
        };
        static const Item items[] = {
            { "music.wav", ".wav" },
            { "俱乐部音乐循环-Freesound.aiff", ".aiff" },
            { "小镇姑娘-陶喆.flac", ".flac" },
            { "Ringtones-耳聆网.ogg", ".ogg" },
            { "摇滚乐_Freesound.m4a", ".m4a" },
            { "萨克斯机.asf", ".asf" },
            { "music-ape.ape", ".ape" },
            { "music-mkv.mkv", ".mkv" },
            { "WavinFlag.aac", ".aac" },
        };
        RealMediaFixture fixture;
        TempFileSet tmp("reject_all");
        for (const Item &item : items) {
            if (!fixture.exists(item.name)) {
                continue;
            }
            std::vector<char> broken = readWholeFile(fixture.mediaPath(item.name));
            REQUIRE(!broken.empty());
            zeroRange(broken, 0, 512);
            std::string path = tmp.make(item.extension);
            REQUIRE(writeWholeFile(path, broken));
            ExtractorOutcome outcome = openExtractor(path, item.extension);
            CHECK(outcome.created == false);
        }
    }

}
