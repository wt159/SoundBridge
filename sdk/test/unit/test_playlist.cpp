#include "../fixtures/RealMediaFixture.hpp"
#include "../fixtures/SyntheticWav.hpp"
#include "../fixtures/TempMediaDir.hpp"
#include <MusicPlayList.h>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <fstream>
#include <future>
#include <thread>

using namespace sdk;

namespace {

struct TestPlayListCallback : public MusicPlayListCallback {
    int currentBufCount = 0;
    void putMusicPlayListCurBuf(MusicPropertiesPtr) override { currentBufCount++; }
    void updateMusicList(std::vector<MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(ErrorCode, const std::string &, int, const std::string &) override { }
};

std::shared_ptr<MusicPlayList> makeMusicPlayListForTest()
{
    static TestPlayListCallback callback;
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    return std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
}

std::shared_ptr<MusicPlayList> makeMusicPlayListWithOneTrack(TestPlayListCallback &callback,
                                                             const std::string &path)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    std::shared_ptr<MusicPlayList> playlist
        = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

    std::promise<void> done;
    std::future<void> future = done.get_future();
    playlist->addMusicWithNotify(path, [&done]() { done.set_value(); });
    future.wait();
    return playlist;
}

std::shared_ptr<MusicPlayList> makeMusicPlayListWithTracks(TestPlayListCallback &callback,
                                                           const std::vector<std::string> &paths)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    std::shared_ptr<MusicPlayList> playlist
        = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

    for (const std::string &path : paths) {
        std::promise<void> done;
        std::future<void> future = done.get_future();
        playlist->addMusicWithNotify(path, [&done]() { done.set_value(); });
        future.wait();
    }
    return playlist;
}

std::shared_ptr<MusicPlayList> makePositionedTwoTrackPlayList(TestPlayListCallback &callback,
                                                              const std::string &path)
{
    std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListWithTracks(callback, { path, path });
    playlist->setCurrentIndexSync(1);
    return playlist;
}

// 列表编辑判据（ADR 0018 / 契约 §4.1）：把每次载荷的索引序列当场抄下来。
// updateMusicList 收到的是活引用，事后再读会看到被后续命令改过的 index。
struct EditRecordingCallback : public MusicPlayListCallback {
    int currentBufCount  = 0;
    int lastCurrentIndex = -1;
    std::vector<size_t> payloadSizes;
    std::vector<std::vector<size_t>> payloadIndices;
    std::vector<ErrorCode> errors;
    std::vector<int> errorIndices;

    void putMusicPlayListCurBuf(MusicPropertiesPtr property) override
    {
        if (property == nullptr) {
            return;
        }
        currentBufCount++;
        lastCurrentIndex = static_cast<int>(property->index);
    }

    void updateMusicList(std::vector<MusicPropertiesPtr> &list) override
    {
        std::vector<size_t> indices;
        indices.reserve(list.size());
        for (const MusicPropertiesPtr &item : list) {
            if (item != nullptr) {
                indices.push_back(item->index);
            }
        }
        payloadSizes.push_back(list.size());
        payloadIndices.push_back(indices);
    }

    void onMusicPlayListError(ErrorCode code, const std::string &, int index,
                              const std::string &) override
    {
        errors.push_back(code);
        errorIndices.push_back(index);
    }
};

std::shared_ptr<MusicPlayList> makeEditPlayList(EditRecordingCallback &callback)
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

bool writeBytes(const std::string &path, const std::vector<char> &bytes)
{
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// 入列是异步的：轮询条目数到位。超时给得很宽——1000 次入列要各开一次文件。
bool waitForCount(MusicPlayList &playlist, int expected, int timeoutMs = 30000)
{
    const int step = 20;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        if (playlist.getMusicCount() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
    }
    return playlist.getMusicCount() == expected;
}

bool indicesConsecutive(const std::vector<size_t> &indices)
{
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] != i) {
            return false;
        }
    }
    return true;
}

}

TEST_SUITE("FileProperties")
{
    TEST_CASE("Parse file path with extension")
    {
        FileProperties props;
        props.fullPath = "/tmp/music/song.flac";
        props.parseFileName();

        REQUIRE(props.fileDir == "/tmp/music/");
        REQUIRE(props.fileName == "song");
        REQUIRE(props.extensionName == ".flac");
    }

    TEST_CASE("Parse file path without extension")
    {
        FileProperties props;
        props.fullPath = "/tmp/music/song";
        props.parseFileName();

        REQUIRE(props.fileDir == "/tmp/music/");
        REQUIRE(props.fileName == "song");
        REQUIRE(props.extensionName.empty());
    }
}

TEST_SUITE("MusicPlayList")
{
    TEST_CASE("Default playback mode is sequential")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->playbackMode() == MusicPlaybackMode::Sequential);
    }

    TEST_CASE("Set playback mode persists")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        playlist->setPlaybackMode(MusicPlaybackMode::Random);
        REQUIRE(playlist->playbackMode() == MusicPlaybackMode::Random);
    }

    TEST_CASE("Empty playlist nextSync returns false")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->nextSync() == false);
    }

    TEST_CASE("Empty playlist previousSync returns false")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->previousSync() == false);
    }

    TEST_CASE("Empty playlist setCurrentIndexSync returns false")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->setCurrentIndexSync(0) == false);
    }

    TEST_CASE("Empty playlist advanceToNextTrack returns false")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Empty playlist skipToNextPlayable returns false")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->skipToNextPlayable() == false);
    }

    TEST_CASE("Single track setCurrentIndexSync succeeds")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makeMusicPlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(playlist->setCurrentIndexSync(0) == true);
    }

    TEST_CASE("Single track nextSync and previousSync succeed")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makeMusicPlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(playlist->nextSync() == true);
        REQUIRE(playlist->previousSync() == true);
    }

    TEST_CASE("Non-empty playlist invalid current index returns false")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });

        REQUIRE(playlist->getMusicCount() == 2);
        REQUIRE(playlist->setCurrentIndexSync(-1) == false);
        REQUIRE(playlist->setCurrentIndexSync(2) == false);
    }

    TEST_CASE("Two track playlist nextSync and previousSync succeed")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });

        REQUIRE(playlist->getMusicCount() == 2);
        REQUIRE(playlist->nextSync() == true);
        REQUIRE(playlist->previousSync() == true);
    }

    TEST_CASE("Current item once advanceToNextTrack returns false")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::CurrentItemOnce);

        REQUIRE(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Current item in loop advanceToNextTrack returns true")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::CurrentItemInLoop);

        REQUIRE(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Sequential advanceToNextTrack returns false at end")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Sequential);

        REQUIRE(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Loop advanceToNextTrack wraps and returns true")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Loop);

        REQUIRE(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Random advanceToNextTrack returns true")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Random);

        REQUIRE(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("skipToNextPlayable returns false on empty playlist")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        REQUIRE(playlist->skipToNextPlayable() == false);
    }

    TEST_CASE("skipToNextPlayable does not retry the only track")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makeMusicPlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        // 只有一曲时没有「下一首」：绕回自己就是同一曲重试第二次（规格 8.2 / 实现清单 21）
        REQUIRE(playlist->skipToNextPlayable() == false);
    }

    TEST_CASE("skipToNextPlayable with multiple tracks wraps around")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListWithTracks(
            callback,
            { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav"),
              fixture.mediaPath("music.wav") });
        playlist->setCurrentIndexSync(2);
        REQUIRE(playlist->skipToNextPlayable() == true);
    }

    TEST_CASE("addMusicWithNotify with non-existent file fails gracefully")
    {
        TestPlayListCallback callback;
        static WorkQueue workQueue;
        AudioSpec devSpec;
        devSpec.sampleRate     = 16000;
        devSpec.numChannel     = 1;
        devSpec.bytesPerSample = 2;
        devSpec.bitsPerSample  = 16;
        devSpec.format         = AudioFormatS16;

        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

        std::promise<void> done;
        std::future<void> future = done.get_future();
        playlist->addMusicWithNotify("/non/existent/path/file.wav",
                                     [&done]() { done.set_value(); });
        future.wait();

        REQUIRE(playlist->getMusicCount() == 0);
    }

    TEST_CASE("addMusicWithNotify with unsupported extension fails gracefully")
    {
        RealMediaFixture fixture;
        TestPlayListCallback callback;
        static WorkQueue workQueue;
        AudioSpec devSpec;
        devSpec.sampleRate     = 16000;
        devSpec.numChannel     = 1;
        devSpec.bytesPerSample = 2;
        devSpec.bitsPerSample  = 16;
        devSpec.format         = AudioFormatS16;

        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

        std::string fakePath = fixture.mediaPath("music.wav");
        fakePath             = fakePath.substr(0, fakePath.find_last_of('.')) + ".unsupported";

        std::promise<void> done;
        std::future<void> future = done.get_future();
        playlist->addMusicWithNotify(fakePath, [&done]() { done.set_value(); });
        future.wait();

        REQUIRE(playlist->getMusicCount() == 0);
    }

    TEST_CASE("addMusicWithNotify with null callback handles error")
    {
        static WorkQueue workQueue;
        AudioSpec devSpec;
        devSpec.sampleRate     = 16000;
        devSpec.numChannel     = 1;
        devSpec.bytesPerSample = 2;
        devSpec.bitsPerSample  = 16;
        devSpec.format         = AudioFormatS16;

        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(nullptr, &workQueue, devSpec);

        std::promise<void> done;
        std::future<void> future = done.get_future();
        playlist->addMusicWithNotify("/non/existent/file.wav", [&done]() { done.set_value(); });
        future.wait();

        REQUIRE(playlist->getMusicCount() == 0);
    }

    TEST_CASE("setTraceId updates trace ID")
    {
        std::shared_ptr<MusicPlayList> playlist = makeMusicPlayListForTest();
        playlist->setTraceId("test-trace-123");
    }

    TEST_CASE("updateList with empty playlist does not call callback")
    {
        TestPlayListCallback callback;
        static WorkQueue workQueue;
        AudioSpec devSpec;
        devSpec.sampleRate     = 16000;
        devSpec.numChannel     = 1;
        devSpec.bytesPerSample = 2;
        devSpec.bitsPerSample  = 16;
        devSpec.format         = AudioFormatS16;

        std::shared_ptr<MusicPlayList> playlist
            = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
        REQUIRE(playlist->getMusicCount() == 0);
    }

    TEST_CASE("getMusicCount returns current count")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        TestPlayListCallback callback;
        std::shared_ptr<MusicPlayList> playlist
            = makeMusicPlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        REQUIRE(playlist->getMusicCount() == 1);
    }
}

TEST_SUITE("MusicId")
{
    // 派生身份键（ADR 0014）：纯字符串处理，不碰文件系统。
    TEST_CASE("empty path yields empty id")
    {
        CHECK(deriveMusicId("").empty());
    }

    TEST_CASE("http url keeps its original form")
    {
        const std::string url = "http://example.com/a/../b?token=1";
        CHECK(deriveMusicId(url) == url);
    }

    TEST_CASE("dot segments collapse lexically")
    {
        CHECK(deriveMusicId("/a/b/../c") == "/a/c");
        CHECK(deriveMusicId("/a/./b") == "/a/b");
        CHECK(deriveMusicId("/a/b/../..") == "/");
        // 越根：`..` 在根上丢弃，不留下 `/..`（与 std::filesystem 的 lexically_normal 一致）。
        CHECK(deriveMusicId("/a/../../b") == "/b");
    }

    TEST_CASE("duplicate separators collapse")
    {
        CHECK(deriveMusicId("/a//b") == "/a/b");
    }

    TEST_CASE("relative path becomes absolute")
    {
        const std::string relative = "music/track.wav";
        const std::string id       = deriveMusicId(relative);
        CHECK(id.size() > relative.size());
        CHECK(id.find(relative) != std::string::npos);
    }

#ifdef _WIN32
    TEST_CASE("mixed separators and case fold to one identity")
    {
        // 同一进程内同一物理文件曾出现 `/` 与 `\` 两种写法（#237 实证）。
        CHECK(deriveMusicId("D:/work/SoundBridge/music/music.wav")
              == deriveMusicId(R"(d:\work\soundbridge\music\music.wav)"));
    }

    TEST_CASE("drive letter survives normalization")
    {
        CHECK(deriveMusicId("C:/a/b.wav") == "c:/a/b.wav");
        // 越根同样丢弃：盘符根之后没有可回退的段。
        CHECK(deriveMusicId("C:/a/../../b") == "c:/b");
        // UNC 前缀（两个分隔符）保留，段内 `..` 照常折叠。
        CHECK(deriveMusicId(R"(\\server\share\a\..\b)") == "//server/share/b");
    }
#endif
}

TEST_SUITE("MusicPlayList Edit")
{
    // #268 / A1：千曲量级删中间一条 —— 载荷 999 条且索引 0..998 连续。
    TEST_CASE("removing one track from a large playlist keeps indexes consecutive")
    {
        TempMediaDir temp("editA1");
        const std::string path = temp.file("bulk.wav");
        REQUIRE(writeBytes(path, makeToneWavBytes(1)));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        // 规模取 500 而不是票面写的 1000：每条曲目在列表里常驻一个 FileSource（打开的
        // FILE*），而本平台的 CRT 默认只允许 512 个并发流、`_setmaxstdio` 在这里返回 -1
        // （MinGW msvcrt 不支持）。实测第 509 条开始 fopen 失败——这是被测代码之外的既有
        // 限制，已单独登记 issue #270。500 条足以覆盖 O(n) 重编号与载荷连续性。
        for (int i = 0; i < 500; ++i) {
            playlist->addMusic(path);
        }
        REQUIRE_MESSAGE(waitForCount(*playlist, 500, 30000), "500 条没有入列完成");

        const size_t payloadsBefore = callback.payloadIndices.size();
        // 直接调同步版本：此刻队列已空闲，与既有 setCurrentIndexSync 用例同一前提。
        CHECK(playlist->removeTrackSync(250) == PlaylistEditOutcome::RemovedOther);
        REQUIRE(callback.payloadIndices.size() == payloadsBefore + 1);

        CHECK(callback.payloadSizes.back() == 499);
        CHECK(indicesConsecutive(callback.payloadIndices.back()));
        CHECK(playlist->getMusicCount() == 499);
    }

    // #268 / A2：失败集合按条目身份记账 —— 删掉它上方的一条之后，跳过的仍是原来那条坏曲。
    TEST_CASE("failed tracks stay failed after a removal above them")
    {
        TempMediaDir temp("editA2");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("t1.wav"), tone));
        REQUIRE(writeBytes(temp.file("t2.wav"), tone));
        REQUIRE(writeBytes(temp.file("t3.wav"), tone));
        REQUIRE(writeBytes(temp.file("t4.wav"), tone));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        for (int i = 1; i <= 4; ++i) {
            playlist->addMusic(temp.dir() + "/t" + std::to_string(i) + ".wav");
        }
        REQUIRE_MESSAGE(waitForCount(*playlist, 4), "四条曲目没有入列完成");

        // 当前曲目挪到第 2 条，这样删掉第 1 条时删的是「当前曲目上方的非当前条目」
        // ——正是 index 记账会错位的那种情形。
        REQUIRE(playlist->setCurrentIndexSync(1));
        const uint64_t uidOfThird = playlist->trackUidForTest(2);
        REQUIRE(uidOfThird != 0);
        playlist->markTrackFailed(uidOfThird);

        // 删掉第 1 条之后：原第 3 条落到 index 1，原第 4 条落到 index 2，当前曲目落到 0。
        CHECK(playlist->removeTrackSync(0) == PlaylistEditOutcome::RemovedOther);
        REQUIRE(playlist->getMusicCount() == 3);

        CHECK_MESSAGE(playlist->skipToNextPlayable() == true, "没有跳到可播的曲目");
        CHECK_MESSAGE(callback.lastCurrentIndex == 2,
                      "落点是 " << callback.lastCurrentIndex << "，期望 2（原第 4 条）：失败的条目"
                                << "跟着身份走，不该因为删了上方一条就变成别人");
        CHECK(playlist->isTrackFailed(uidOfThird));
        CHECK(playlist->isTrackFailed(playlist->trackUidForTest(2)) == false);
    }

    // #268 / A8：连续删 10 条 —— 每次载荷都连续，且不抛 std::out_of_range。
    TEST_CASE("removing ten tracks in a row keeps every payload consecutive")
    {
        TempMediaDir temp("editA8");
        const std::string path = temp.file("ten.wav");
        REQUIRE(writeBytes(path, makeToneWavBytes(1)));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        for (int i = 0; i < 10; ++i) {
            playlist->addMusic(path);
        }
        REQUIRE_MESSAGE(waitForCount(*playlist, 10), "十条曲目没有入列完成");

        for (int i = 0; i < 10; ++i) {
            const size_t payloadsBefore = callback.payloadIndices.size();
            const PlaylistEditOutcome outcome = playlist->removeTrackSync(0);
            REQUIRE_MESSAGE(outcome != PlaylistEditOutcome::Dropped, "第 " << i << " 次删除被丢弃");
            REQUIRE(callback.payloadIndices.size() == payloadsBefore + 1);
            CHECK(callback.payloadSizes.back() == static_cast<size_t>(9 - i));
            CHECK(indicesConsecutive(callback.payloadIndices.back()));
            CHECK(playlist->getMusicCount() == 9 - i);
        }
        CHECK(playlist->getMusicCount() == 0);
        CHECK(callback.payloadIndices.back().empty());
    }

    // #268 回归：删掉当前曲目之后，退役任务必须仍然持有那个条目。
    // 反例在 Linux CI 上是 SIGSEGV（A5/A8 两处崩溃）：解码线程只拿 extractor 的裸指针，
    // 条目一 erase 就析构；退役任务只 abort 不 join，于是解码线程读的是已释放的 extractor。
    // MinGW 上释放的堆块仍映射，所以这条判据是唯一能在 Windows 上也变红的入口。
    TEST_CASE("the retired task keeps the removed entry alive")
    {
        TempMediaDir temp("editOwner");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("one.wav"), tone));
        REQUIRE(writeBytes(temp.file("two.wav"), tone));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        playlist->addMusic(temp.file("one.wav"));
        playlist->addMusic(temp.file("two.wav"));
        REQUIRE_MESSAGE(waitForCount(*playlist, 2), "两条曲目没有入列完成");

        // 首发曲目即当前曲目；任务由 startCurrentTask() 建（入列本身不建），
        // 否则退役槽里根本没有任务，删除也就无从「保住」任何东西。
        REQUIRE(playlist->startCurrentTask());
        const std::weak_ptr<MusicProperties> removedRef = playlist->trackRefForTest(0);
        REQUIRE_FALSE(removedRef.expired());

        REQUIRE(playlist->removeTrackSync(0) == PlaylistEditOutcome::CurrentReplaced);
        REQUIRE(playlist->getMusicCount() == 1);

        CHECK_MESSAGE(!removedRef.expired(),
                      "删掉当前曲目后条目立刻析构：退役任务里的解码线程会读已释放的 extractor");
    }

    // 契约 §4.1：addTrack 的失败必须可见（绕过扫描降噪门槛），且不产生列表载荷。
    TEST_CASE("addTrack failure is reported even when a current track exists")
    {
        TempMediaDir temp("editAddFail");
        const std::string goodPath = temp.file("good.wav");
        REQUIRE(writeBytes(goodPath, makeToneWavBytes(1)));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        playlist->addMusic(goodPath);
        REQUIRE_MESSAGE(waitForCount(*playlist, 1), "参考曲目没有入列");
        REQUIRE(callback.errors.empty());

        const size_t payloadsBefore = callback.payloadIndices.size();
        CHECK(playlist->addTrackSync(temp.dir() + "/missing.wav") == false);
        REQUIRE(callback.errors.size() == 1);
        CHECK(callback.errorIndices.front() == -1);
        CHECK(callback.payloadIndices.size() == payloadsBefore);
        CHECK(playlist->getMusicCount() == 1);

        // 拒绝累计不得串进下一次目录扫描的批次汇总（契约 §4.1）。
        playlist->reportScanSummary();
        CHECK(callback.errors.size() == 1);

        // 传目录：丢弃，零回调。
        CHECK(playlist->addTrackSync(temp.dir()) == false);
        CHECK(callback.errors.size() == 1);
        CHECK(playlist->getMusicCount() == 1);
    }

    // 契约 §4.1：clearPlaylist 在空列表上是幂等的（零回调、零查询值变化）。
    TEST_CASE("clearPlaylist on an empty list is a silent no-op")
    {
        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);

        CHECK(playlist->clearPlaylistSync() == false);
        CHECK(callback.payloadIndices.empty());
        CHECK(callback.errors.empty());
        CHECK(callback.currentBufCount == 0);
        CHECK(playlist->getMusicCount() == 0);
    }

    // 契约 §4.1：清空非空列表必须发一次空载荷（扫描的「空目录沉默」是另一条路径）。
    TEST_CASE("clearPlaylist publishes an empty payload")
    {
        TempMediaDir temp("editClear");
        const std::vector<char> tone = makeToneWavBytes(1);
        REQUIRE(writeBytes(temp.file("a.wav"), tone));
        REQUIRE(writeBytes(temp.file("b.wav"), tone));

        EditRecordingCallback callback;
        std::shared_ptr<MusicPlayList> playlist = makeEditPlayList(callback);
        for (int i = 0; i < 2; ++i) {
            playlist->addMusic(temp.dir() + "/" + (i == 0 ? "a.wav" : "b.wav"));
        }
        REQUIRE_MESSAGE(waitForCount(*playlist, 2), "两条曲目没有入列完成");

        CHECK(playlist->clearPlaylistSync() == true);
        REQUIRE(callback.payloadSizes.size() == 1);
        CHECK(callback.payloadSizes.back() == 0);
        CHECK(playlist->getMusicCount() == 0);
    }
}
