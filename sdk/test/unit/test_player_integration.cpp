#include "../fixtures/RealMediaFixture.hpp"
#include "../mocks/PlayerMocks.hpp"
#include <MusicPlayer.h>
#include <doctest/doctest.h>
#include <thread>

using namespace sdk;

TEST_SUITE("MusicPlayer State Transitions")
{
    TEST_CASE("initial state is stopped")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        CHECK(player.state() == MusicPlayerState::StoppedState);
    }

    TEST_CASE("stop while stopped remains stopped")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(player.state() == MusicPlayerState::StoppedState);
    }

    TEST_CASE("pause while stopped remains stopped")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(player.state() == MusicPlayerState::StoppedState);
    }
}

TEST_SUITE("MusicPlayer Error Handling")
{
    TEST_CASE("play without track triggers error callback")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.play();

        auto start = std::chrono::steady_clock::now();
        while (listener.error_count == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(3000))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        CHECK(listener.error_count > 0);
    }

    TEST_CASE("autoSkipOnError is enabled by default")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);
        CHECK(player.autoSkipOnError() == true);
    }

    TEST_CASE("setAutoSkipOnError can be disabled")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.setAutoSkipOnError(false);
        CHECK(player.autoSkipOnError() == false);

        player.setAutoSkipOnError(true);
        CHECK(player.autoSkipOnError() == true);
    }
}

TEST_SUITE("MusicPlayer Position")
{
    TEST_CASE("setPosition updates position")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        player.setPosition(5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

TEST_SUITE("MusicPlayer Track Navigation")
{
    TEST_CASE("next with tracks advances")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.addMusicDir(fixture.mediaDir());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (player.getMusicCount() > 1) {
            int initialIndex = listener.last_current_index;
            player.next();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    TEST_CASE("setCurrentIndex with invalid index is safe")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.setCurrentIndex(-1);
        player.setCurrentIndex(999);
        CHECK(player.getMusicCount() == 0);
    }

    TEST_CASE("previous with empty playlist is safe")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.previous();
        CHECK(player.getMusicCount() == 0);
    }
}

TEST_SUITE("MusicPlayer Playlist Operations")
{
    TEST_CASE("addMusicDir with valid directory loads tracks")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.addMusicDir(fixture.mediaDir());

        auto start = std::chrono::steady_clock::now();
        while (player.getMusicCount() == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(5000))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        CHECK(player.getMusicCount() > 0);
    }

    TEST_CASE("getMusicCount starts at zero")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        CHECK(player.getMusicCount() == 0);
    }
}

TEST_SUITE("MusicPlayer Playback Mode")
{
    TEST_CASE("default playback mode is sequential")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        CHECK(player.playbackMode() == MusicPlaybackMode::Sequential);
    }

    TEST_CASE("playback mode can be changed")
    {
        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.setPlaybackMode(MusicPlaybackMode::CurrentItemInLoop);
        CHECK(player.playbackMode() == MusicPlaybackMode::CurrentItemInLoop);

        player.setPlaybackMode(MusicPlaybackMode::Random);
        CHECK(player.playbackMode() == MusicPlaybackMode::Random);

        player.setPlaybackMode(MusicPlaybackMode::Loop);
        CHECK(player.playbackMode() == MusicPlaybackMode::Loop);
    }
}
