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
#else
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
    int currentBufCount = 0;
    int lastCurrentIndex = -1;

    void putMusicPlayListCurBuf(sdk::MusicPropertiesPtr property) override
    {
        ++currentBufCount;
        lastCurrentIndex = property ? static_cast<int>(property->index) : -1;
    }
    void updateMusicList(std::vector<sdk::MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(sdk::ErrorCode, const std::string &, int, const std::string &)
        override
    { }
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
}
