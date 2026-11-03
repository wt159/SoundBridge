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
        if (!fixture.exists("像我这样的人-毛不易.128.mp3")) {
            return;
        }
        TempFileSet tmp("player");
        std::string dir = tmp.makeDir();
        REQUIRE(!dir.empty());
        std::vector<char> data = readWholeFile(fixture.mediaPath("像我这样的人-毛不易.128.mp3"));
        REQUIRE(!data.empty());
        zeroRange(data, data.size() * 2 / 100, 4096);
        // 单曲目录：autoSkip 因「没有下一首」关闭，停下来的位置是确定的
        REQUIRE(writeWholeFile(dir + "/broken.mp3", data));

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
}
