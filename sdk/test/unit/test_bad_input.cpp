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
        std::string path = m_dir + "/sb_badinput_" + m_tag + "_" + std::to_string(++s_counter)
            + extension;
        m_files.push_back(path);
        return path;
    }

    std::string makeDir()
    {
        std::string path = m_dir + "/sb_badinput_" + m_tag + "_dir" + std::to_string(++s_counter);
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

StreamMeasurement measureStream(const std::string &path, const char *extension, int timeoutMs)
{
    StreamMeasurement measurement;
    FileSource source(path.c_str());
    if (source.initCheck() != sdk_utils::OK) {
        return measurement;
    }
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(&source, extension, true));
    if (!extractor || extractor->initCheck() != sdk_utils::OK) {
        return measurement;
    }
    measurement.opened = true;

    AudioRingBuffer ring(AudioRingBuffer::kDefaultCapacity);
    AudioStreamDecoder decoder(&ring, makeDevSpec());
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
}
