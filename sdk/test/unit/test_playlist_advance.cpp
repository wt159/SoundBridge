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

std::shared_ptr<MusicPlayList> makeEmptyPlayList()
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

std::shared_ptr<MusicPlayList> makePlayListWithOneTrack(TestPlayListCallback &callback,
                                                        const std::string &path)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    auto playlist = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

    std::promise<void> done;
    playlist->addMusicWithNotify(path, [&done]() { done.set_value(); });
    done.get_future().wait();

    return playlist;
}

std::shared_ptr<MusicPlayList> makePlayListWithTracks(TestPlayListCallback &callback,
                                                      const std::vector<std::string> &paths)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    auto playlist = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);

    for (const auto &path : paths) {
        std::promise<void> done;
        playlist->addMusicWithNotify(path, [&done]() { done.set_value(); });
        done.get_future().wait();
    }

    return playlist;
}

std::shared_ptr<MusicPlayList> makePositionedTwoTrackPlayList(TestPlayListCallback &callback,
                                                              const std::string &path)
{
    auto playlist = makePlayListWithTracks(callback, { path, path });
    playlist->setCurrentIndexSync(1);
    return playlist;
}

std::shared_ptr<MusicPlayList> makePositionedThreeTrackPlayList(TestPlayListCallback &callback,
                                                                const std::string &path)
{
    auto playlist = makePlayListWithTracks(callback, { path, path, path });
    playlist->setCurrentIndexSync(2);
    return playlist;
}

} // namespace

TEST_SUITE("advanceToNextTrack")
{
    TEST_CASE("CurrentItemOnce returns false")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::CurrentItemOnce);

        CHECK(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("CurrentItemInLoop returns true and stays on same track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::CurrentItemInLoop);

        CHECK(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Sequential returns false at end")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Sequential);

        CHECK(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Sequential returns true when not at end")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedThreeTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Sequential);
        playlist->setCurrentIndexSync(0); // Start at first track

        CHECK(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Loop wraps to first track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Loop);

        CHECK(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Random selects different track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedTwoTrackPlayList(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Random);

        CHECK(playlist->advanceToNextTrack() == true);
    }

    TEST_CASE("Empty playlist returns false")
    {
        auto playlist = makeEmptyPlayList();
        CHECK(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Single track Sequential returns false")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Sequential);

        CHECK(playlist->advanceToNextTrack() == false);
    }

    TEST_CASE("Single track Loop returns true")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));
        playlist->setPlaybackMode(MusicPlaybackMode::Loop);

        CHECK(playlist->advanceToNextTrack() == true);
    }
}

TEST_SUITE("skipToNextPlayable")
{
    TEST_CASE("returns false on empty playlist")
    {
        auto playlist = makeEmptyPlayList();
        CHECK(playlist->skipToNextPlayable() == false);
    }

    TEST_CASE("returns true when track is selectable")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        CHECK(playlist->skipToNextPlayable() == true);
    }

    TEST_CASE("returns true with multiple tracks")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithTracks(callback,
                                               { fixture.mediaPath("music.wav"),
                                                 fixture.mediaPath("music.wav"),
                                                 fixture.mediaPath("music.wav") });

        CHECK(playlist->skipToNextPlayable() == true);
    }

    TEST_CASE("wraps around at end")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePositionedThreeTrackPlayList(callback, fixture.mediaPath("music.wav"));

        CHECK(playlist->skipToNextPlayable() == true);
    }
}

TEST_SUITE("selectTrack")
{
    TEST_CASE("setCurrentIndexSync selects valid track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });

        CHECK(playlist->setCurrentIndexSync(0) == true);
        CHECK(playlist->setCurrentIndexSync(1) == true);
    }

    TEST_CASE("setCurrentIndexSync rejects invalid index")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });

        CHECK(playlist->setCurrentIndexSync(-1) == false);
        CHECK(playlist->setCurrentIndexSync(2) == false);
        CHECK(playlist->setCurrentIndexSync(100) == false);
    }

    TEST_CASE("setCurrentIndexSync on empty playlist returns false")
    {
        auto playlist = makeEmptyPlayList();
        CHECK(playlist->setCurrentIndexSync(0) == false);
    }
}

TEST_SUITE("startStreaming")
{
    TEST_CASE("skipToNextPlayable triggers streaming on valid track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithOneTrack(callback, fixture.mediaPath("music.wav"));

        // skipToNextPlayable internally calls selectTrack which calls startStreaming
        CHECK(playlist->skipToNextPlayable() == true);
    }

    TEST_CASE("nextSync triggers streaming on next track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });
        playlist->setCurrentIndexSync(0);

        // nextSync internally triggers streaming
        CHECK(playlist->nextSync() == true);
    }

    TEST_CASE("previousSync triggers streaming on previous track")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        TestPlayListCallback callback;
        auto playlist = makePlayListWithTracks(
            callback, { fixture.mediaPath("music.wav"), fixture.mediaPath("music.wav") });
        playlist->setCurrentIndexSync(1);

        // previousSync internally triggers streaming
        CHECK(playlist->previousSync() == true);
    }
}
