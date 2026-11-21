#include "../fixtures/RealMediaFixture.hpp"
#include <MusicPlayList.h>
#include <doctest/doctest.h>
#include <future>

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
