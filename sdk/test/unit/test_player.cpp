#include "../fixtures/RealMediaFixture.hpp"
#include "../mocks/PlayerMocks.hpp"
#include <AudioCommon.hpp>
#include <doctest/doctest.h>
#include <future>
#include <thread>

TEST_SUITE("PlayerCallbacks")
{
    TEST_CASE("MockPlayerCallbacks default values")
    {
        MockPlayerCallbacks callback;
        CHECK(callback.state_change_count == 0);
        CHECK(callback.track_change_count == 0);
        CHECK(callback.error_count == 0);
        CHECK(callback.last_track_index == -1);
    }

    TEST_CASE("MockPlayerCallbacks reset")
    {
        MockPlayerCallbacks callback;
        callback.error_count        = 5;
        callback.state_change_count = 3;
        callback.reset();
        CHECK(callback.error_count == 0);
        CHECK(callback.state_change_count == 0);
    }
}

TEST_SUITE("MusicPlayerListener")
{
    TEST_CASE("MockMusicPlayerListener default values")
    {
        MockMusicPlayerListener listener;
        CHECK(listener.state_change_count == 0);
        CHECK(listener.error_count == 0);
        CHECK(listener.last_current_index == -1);
    }

    TEST_CASE("MockMusicPlayerListener reset")
    {
        MockMusicPlayerListener listener;
        listener.error_count = 5;
        listener.reset();
        CHECK(listener.error_count == 0);
    }
}

TEST_SUITE("LegacyMusicPlayer")
{
    TEST_CASE("Default playback mode is sequential")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);
        CHECK(player.playbackMode() == sdk::MusicPlaybackMode::Sequential);
    }

    TEST_CASE("Set and verify playback mode")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);

        player.setPlaybackMode(sdk::MusicPlaybackMode::CurrentItemOnce);
        CHECK(player.playbackMode() == sdk::MusicPlaybackMode::CurrentItemOnce);

        player.setPlaybackMode(sdk::MusicPlaybackMode::CurrentItemInLoop);
        CHECK(player.playbackMode() == sdk::MusicPlaybackMode::CurrentItemInLoop);

        player.setPlaybackMode(sdk::MusicPlaybackMode::Random);
        CHECK(player.playbackMode() == sdk::MusicPlaybackMode::Random);

        player.setPlaybackMode(sdk::MusicPlaybackMode::Loop);
        CHECK(player.playbackMode() == sdk::MusicPlaybackMode::Loop);
    }

    TEST_CASE("Play without track reports error")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        sdk::MusicPlayer player(&listener, logDir);

        player.play();

        auto start = std::chrono::steady_clock::now();
        while (listener.error_count == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(3000)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CHECK(listener.error_count > 0);
    }
}

TEST_SUITE("PublicPlayer")
{
    TEST_CASE("Default playback mode is sequential")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);
        CHECK(player.playbackMode() == soundbridge::PlaybackMode::Sequential);
    }

    TEST_CASE("Set and verify playback mode")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);

        player.setPlaybackMode(soundbridge::PlaybackMode::SingleOnce);
        CHECK(player.playbackMode() == soundbridge::PlaybackMode::SingleOnce);

        player.setPlaybackMode(soundbridge::PlaybackMode::SingleLoop);
        CHECK(player.playbackMode() == soundbridge::PlaybackMode::SingleLoop);

        player.setPlaybackMode(soundbridge::PlaybackMode::Random);
        CHECK(player.playbackMode() == soundbridge::PlaybackMode::Random);

        player.setPlaybackMode(soundbridge::PlaybackMode::Loop);
        CHECK(player.playbackMode() == soundbridge::PlaybackMode::Loop);
    }

    TEST_CASE("Play without track reports error")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);

        player.play();

        auto start = std::chrono::steady_clock::now();
        while (callback.error_count == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(3000)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CHECK(callback.error_count > 0);
    }

    TEST_CASE("Add music directory loads tracks")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);

        std::string mediaDir = fixture.mediaDir();
        player.addMusicDirectory(mediaDir);

        auto start = std::chrono::steady_clock::now();
        while (player.trackCount() == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(5000)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        CHECK(player.trackCount() > 0);
    }

    TEST_CASE("Position starts at zero")
    {
        MockPlayerCallbacks callback;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(&callback, config);

        CHECK(player.position() == 0);
    }
}
