// 扫描线程模型与请求级状态的判据（ADR 0021；地图 #313 → 执行票 #320 / #321 / #322）。
//
// 每条判据对应 ADR 的一条决策，且都能在无真实慢盘的环境里确定性复现：
// 用 FileSource::setProbeDelayForTest 造「扫描中」窗口，用句柄计数观察预算，
// 用 FakeAudioDevice 让「扫描期 play()」不依赖真实声卡。
#include "../fixtures/SyntheticWav.hpp"
#include "../fixtures/TempMediaDir.hpp"
#include "../mocks/PlayerMocks.hpp"
#include <FileSearch.h>
#include <FileSource.h>
#include <IAudioDevice.h>
#include <MusicPlayList.h>
#include <MusicPlayer.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sdk;

namespace {

bool writeBytes(const std::string &path, const std::vector<char> &bytes)
{
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::vector<char> readBytes(const std::string &path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

// 把 WAV 头清零：扩展名仍注册，createExtractor 会拒绝文件头（ExtractorInitFailed），
// 是「扫描拒绝」最轻的构造方式。
bool breakHeader(const std::string &path)
{
    std::vector<char> bytes = readBytes(path);
    if (bytes.size() < 512) {
        return false;
    }
    for (size_t i = 0; i < 512; ++i) {
        bytes[i] = 0;
    }
    return writeBytes(path, bytes);
}

// 探测延迟的 RAII 开关（纪律同 ScopedOpenDelay）：默认 0、测完必须归零。
struct ScopedProbeDelay {
    explicit ScopedProbeDelay(uint32_t ms) { FileSource::setProbeDelayForTest(ms); }
    ~ScopedProbeDelay() { FileSource::setProbeDelayForTest(0); }
};

bool waitFor(const std::function<bool()> &predicate, int timeoutMs, int stepMs = 20)
{
    for (int waited = 0; waited < timeoutMs; waited += stepMs) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return predicate();
}

// 列表级判据的回调：保留最后一次载荷的路径序（updateMusicList 收的是活引用，
// 事后再读会看到后续命令改过的值，所以当场拷贝）。
struct ScanRecordingCallback : public MusicPlayListCallback {
    std::atomic<int> currentBufCount { 0 };
    std::atomic<int> publishCount { 0 };
    std::atomic<int> lastPublishedSize { 0 };
    std::atomic<int> errorCount { 0 };
    // 非 0 时在 updateMusicList 里原地睡这么久：用来把控制队列占住，
    // 让扫描结果积压在队列里（迟到结果判据的确定性窗口）。
    std::atomic<int> blockPublishMs { 0 };
    mutable std::mutex mutex;
    std::vector<std::string> publishedPaths;
    std::vector<int> errorIndices;

    void putMusicPlayListCurBuf(MusicPropertiesPtr) override { currentBufCount.fetch_add(1); }

    void updateMusicList(std::vector<MusicPropertiesPtr> &list) override
    {
        const int blockMs = blockPublishMs.load(std::memory_order_acquire);
        if (blockMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(blockMs));
        }
        publishCount.fetch_add(1);
        lastPublishedSize.store(static_cast<int>(list.size()));
        std::vector<std::string> paths;
        paths.reserve(list.size());
        for (const MusicPropertiesPtr &item : list) {
            if (item != nullptr) {
                paths.push_back(item->fileProperties.fullPath);
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        publishedPaths.swap(paths);
    }

    void onMusicPlayListError(ErrorCode, const std::string &, int index,
                              const std::string &) override
    {
        errorCount.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        errorIndices.push_back(index);
    }

    std::vector<std::string> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return publishedPaths;
    }
};

std::shared_ptr<MusicPlayList> makePlayList(ScanRecordingCallback &callback)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;
    return std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
}

bool waitForCount(MusicPlayList &playlist, int expected, int timeoutMs = 15000)
{
    return waitFor([&playlist, expected]() { return playlist.getMusicCount() == expected; },
                   timeoutMs);
}

bool waitForAtLeast(MusicPlayList &playlist, int expected, int timeoutMs = 15000)
{
    return waitFor([&playlist, expected]() { return playlist.getMusicCount() >= expected; },
                   timeoutMs);
}

std::vector<std::string> makeToneFiles(TempMediaDir &temp, int count)
{
    std::vector<std::string> paths;
    const std::vector<char> tone = makeToneWavBytes(1);
    for (int i = 0; i < count; ++i) {
        const std::string path = temp.file(std::string("t") + std::to_string(i) + ".wav");
        if (!writeBytes(path, tone)) {
            return {};
        }
        paths.push_back(path);
    }
    return paths;
}

// 不依赖真实声卡：open/start/write 全部成功，队列深度恒 0。
// 播放链（解码线程 → 消费线程 → 设备）保持真实，位置仍由解码器读计数推进。
class FakeAudioDevice : public IAudioDevice {
public:
    std::vector<AudDevPair> getDeviceList() override { return {}; }

    int getDeviceSpec(AudioSpec &spec) override
    {
        spec.sampleRate     = 44100;
        spec.numChannel     = 2;
        spec.bytesPerSample = 2;
        spec.bitsPerSample  = 16;
        spec.format         = AudioFormatS16;
        spec.durationMs     = 0;
        return 0;
    }

    int selectDevice(uint64_t) override { return 0; }
    int open() override
    {
        m_open.store(true);
        return 0;
    }
    int close() override
    {
        m_open.store(false);
        return 0;
    }
    bool isOpen() const override { return m_open.load(); }
    int start() override { return m_open.load() ? 0 : -1; }
    int stop() override { return 0; }
    int write(const void *, size_t) override { return 0; }
    size_t getQueuedBytes() const override { return 0; }
    void clearQueue() override { }

private:
    std::atomic<bool> m_open { false };
};

} // namespace

TEST_SUITE("Scan Thread Model")
{
    // ADR 0021 决策 1/3/5：扫描结束后条数 == 文件数、顺序 == 遍历顺序（index 连续），
    // 载荷只在末尾发布一次；扫描自身句柄峰值仍是 1（正对照），结束后归零。
    TEST_CASE("scan inserts every file in traversal order and releases every handle")
    {
        TempMediaDir temp("scanOrder");
        REQUIRE(makeToneFiles(temp, 5).size() == 5);
        const std::vector<std::string> expected = recursiveFileSearch(temp.dir());
        REQUIRE(expected.size() == 5);

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        FileSource::resetHandleStatsForTest();

        playlist->scanDirectory(temp.dir());
        REQUIRE_MESSAGE(waitForCount(*playlist, 5), "扫描没有把五个文件全部入列");

        const std::vector<std::string> published = callback.snapshot();
        REQUIRE(published.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            CHECK(published[i] == expected[i]);
            std::shared_ptr<MusicProperties> entry = playlist->trackRefForTest(i).lock();
            REQUIRE(entry != nullptr);
            CHECK(entry->index == i);
            CHECK(entry->fileProperties.fullPath == expected[i]);
        }
        CHECK(callback.publishCount.load() == 1);
        CHECK(callback.lastPublishedSize.load() == 5);
        CHECK(callback.errorCount.load() == 0);
        CHECK(FileSource::peakOpenHandleCount() == 1);
        CHECK(FileSource::openHandleCount() == 0);
    }

    // ADR 0021 决策 1/5：扫描线程与单条入列（控制队列）并发探测时，
    // 句柄峰值不超过 2（各持一个），全部结束后归零。
    TEST_CASE("a concurrent single add keeps the scan window at no more than two handles")
    {
        TempMediaDir scanDir("scanTwo");
        TempMediaDir singleDir("scanTwoSingle");
        REQUIRE(makeToneFiles(scanDir, 8).size() == 8);
        const std::vector<char> tone = makeToneWavBytes(1);
        const std::string singlePath = singleDir.file("extra.wav");
        REQUIRE(writeBytes(singlePath, tone));

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        ScopedProbeDelay delay(25);
        FileSource::resetHandleStatsForTest();

        playlist->scanDirectory(scanDir.dir());
        REQUIRE_MESSAGE(waitForAtLeast(*playlist, 1), "扫描没有产出任何结果");
        playlist->addTrack(singlePath);

        REQUIRE_MESSAGE(waitForCount(*playlist, 9), "扫描与单条入列没有全部完成");
        CHECK(FileSource::peakOpenHandleCount() <= 2);
        CHECK(FileSource::openHandleCount() == 0);
    }

    // ADR 0021 决策 6：扫描中途 clearPlaylist 是终态命令——列表保持为空、
    // 迟到结果不得把它重新填起来、被拒文件不得借取消路径产出汇总回调。
    TEST_CASE("clearPlaylist during a scan leaves no tracks, no errors and no handles")
    {
        TempMediaDir temp("scanClear");
        REQUIRE(makeToneFiles(temp, 20).size() == 20);
        const std::vector<std::string> order = recursiveFileSearch(temp.dir());
        REQUIRE(order.size() == 20);
        // 第二个文件做成坏头：首个成功条目已登记为当前曲目 ⇒ 它只进拒绝计数，
        // 不产生逐文件回调。取消时若误发汇总，这条拒绝会立刻变成一次 index<0 的 error。
        REQUIRE(breakHeader(order[1]));

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        ScopedProbeDelay delay(20);

        playlist->scanDirectory(temp.dir());
        REQUIRE_MESSAGE(waitForAtLeast(*playlist, 3), "扫描没有走到坏文件之后");
        playlist->clearPlaylist();

        REQUIRE_MESSAGE(waitForCount(*playlist, 0), "清空没有生效");
        // 稳定窗口：取消后的迟到结果必须被丢弃，而不是把列表重新填起来。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(playlist->getMusicCount() == 0);
        CHECK(callback.errorCount.load() == 0);
        CHECK(waitFor([]() { return FileSource::openHandleCount() == 0; }, 5000));
    }

    // ADR 0021 决策 6「第二次 addMusicDir 后者胜」：第一次扫描**已经回投**的结果
    // 也必须在插入时被代际守卫丢弃（不是只靠扫描线程提前退出）。
    TEST_CASE("a second scan supersedes the first and drops its late results")
    {
        TempMediaDir firstDir("scanFirst");
        TempMediaDir secondDir("scanSecond");
        TempMediaDir seedDir("scanSeed");
        REQUIRE(makeToneFiles(firstDir, 10).size() == 10);
        REQUIRE(makeToneFiles(secondDir, 2).size() == 2);
        const std::vector<char> tone = makeToneWavBytes(1);
        const std::string seedPath   = seedDir.file("seed.wav");
        REQUIRE(writeBytes(seedPath, tone));

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        playlist->addTrack(seedPath);
        REQUIRE_MESSAGE(waitForCount(*playlist, 1), "参考曲目没有入列");

        ScopedProbeDelay delay(30);
        // 用一次慢发布把控制队列占住：第一次扫描的结果回投后只能积压在队列里，
        // 第二次扫描在这期间取消它——迟到结果必须被插入时的代际守卫丢弃。
        callback.blockPublishMs.store(400);
        playlist->updateList();
        playlist->scanDirectory(firstDir.dir());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        playlist->scanDirectory(secondDir.dir());
        callback.blockPublishMs.store(0);

        REQUIRE_MESSAGE(waitForCount(*playlist, 3), "第二次扫描没有完成");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(playlist->getMusicCount() == 3);
        const std::vector<std::string> published = callback.snapshot();
        REQUIRE(published.size() == 3);
        CHECK(published[0] == seedPath);
        for (size_t i = 1; i < published.size(); ++i) {
            CHECK(published[i].compare(0, secondDir.dir().size(), secondDir.dir()) == 0);
        }
        CHECK(callback.errorCount.load() == 0);
    }

    // ADR 0021 决策 6：取消位在**每个文件边界**检查 ⇒ 执行器在一个探测内就能接下一单。
    // 若检查只在遍历结束后做，第二次扫描要等第一次把剩余文件全部探测完才启动。
    TEST_CASE("cancelling a scan frees the executor within one probe")
    {
        TempMediaDir longDir("scanLong");
        TempMediaDir nextDir("scanNext");
        REQUIRE(makeToneFiles(longDir, 20).size() == 20);
        REQUIRE(makeToneFiles(nextDir, 1).size() == 1);

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        ScopedProbeDelay delay(60);

        playlist->scanDirectory(longDir.dir());
        REQUIRE_MESSAGE(waitForAtLeast(*playlist, 1), "长扫描没有产出结果");
        playlist->clearPlaylist();
        REQUIRE_MESSAGE(waitForCount(*playlist, 0), "清空没有生效");

        // 长扫描剩余 19 × 60 ms ≈ 1.1 s；取消及时的话，下一次扫描的首个结果
        // 应在一个探测延迟（≈60 ms）量级内出现。
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        playlist->scanDirectory(nextDir.dir());
        REQUIRE_MESSAGE(waitForCount(*playlist, 1, 5000), "取消后执行器没有接下一次扫描");
        const long long elapsedMs
            = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - begin)
                  .count();
        CHECK_MESSAGE(elapsedMs < 800, "取消后执行器被上一次扫描拖住了 " << elapsedMs << " ms");
    }

    // ADR 0021 决策 6：拒绝计数下沉到请求 ⇒ 被取消批次的拒绝不得出现在下一次汇总里。
    TEST_CASE("a cancelled scan's rejections never reach the next summary")
    {
        TempMediaDir rejectedDir("scanRejectA");
        TempMediaDir cleanDir("scanRejectB");
        REQUIRE(makeToneFiles(rejectedDir, 20).size() == 20);
        REQUIRE(makeToneFiles(cleanDir, 2).size() == 2);
        const std::vector<std::string> order = recursiveFileSearch(rejectedDir.dir());
        REQUIRE(order.size() == 20);
        REQUIRE(breakHeader(order[1]));

        ScanRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makePlayList(callback);
        ScopedProbeDelay delay(20);

        playlist->scanDirectory(rejectedDir.dir());
        REQUIRE_MESSAGE(waitForAtLeast(*playlist, 3), "第一批没有走到坏文件之后");
        playlist->clearPlaylist();
        REQUIRE_MESSAGE(waitForCount(*playlist, 0), "清空没有生效");

        playlist->scanDirectory(cleanDir.dir());
        REQUIRE_MESSAGE(waitForCount(*playlist, 2), "第二批没有完成");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // 第二批零拒绝 ⇒ 汇总不得产任何回调；第一批的拒绝计数不得串台。
        CHECK(callback.errorCount.load() == 0);
    }

    // ADR 0021 决策 1/2 + #319 裁决：扫描期 play() 作用于「此刻已入列」的列表，
    // 后续插入不打断播放（当前曲目与索引回调都不变）。
    TEST_CASE("play during a scan starts an inserted track and later inserts do not interrupt it")
    {
        TempMediaDir temp("scanPlay");
        REQUIRE(makeToneFiles(temp, 8).size() == 8);

        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);
        std::shared_ptr<FakeAudioDevice> device = std::make_shared<FakeAudioDevice>();
        REQUIRE(player.setAudioDeviceForTest(device));

        ScopedProbeDelay delay(60);
        player.addMusicDir(temp.dir());
        REQUIRE_MESSAGE(waitFor([&player]() { return player.getMusicCount() >= 1; }, 5000),
                        "扫描没有产出可播条目");

        player.play();
        REQUIRE_MESSAGE(
            waitFor([&player]() { return player.state() == MusicPlayerState::PlayingState; },
                    5000),
            "扫描期 play() 没有起播已入列的曲目");
        // 起播必须发生在扫描结束之前：若探测仍排在控制队列里，play() 会排在全部
        // 探测之后，等它生效时列表已经满了（变异点：探测回到 asyncRunTask）。
        CHECK_MESSAGE(player.getMusicCount() < 8,
                      "play() 等到扫描结束才生效，控制面仍被探测占住");
        REQUIRE_MESSAGE(
            waitFor([&listener]() { return listener.position_change_count.load() > 0; }, 3000),
            "起播后没有位置回调");

        const int indexAtPlay        = listener.last_current_index.load();
        const int indexChangesAtPlay = listener.index_change_count.load();
        REQUIRE_MESSAGE(waitFor([&player]() { return player.getMusicCount() == 8; }, 10000),
                        "扫描没有在播放期间完成");

        CHECK(player.state() == MusicPlayerState::PlayingState);
        CHECK(listener.last_current_index.load() == indexAtPlay);
        CHECK(listener.index_change_count.load() == indexChangesAtPlay);
        CHECK(listener.error_count.load() == 0);
        player.stop();
    }
}
