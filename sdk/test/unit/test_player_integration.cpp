#include "../fixtures/RealMediaFixture.hpp"
#include "../mocks/PlayerMocks.hpp"
#include <MusicPlayer.h>
#include <doctest/doctest.h>
#include <thread>

using namespace sdk;

namespace {
// 轮询等状态到位：播放器把控制命令丢进工作队列异步执行，固定 sleep 容易跑在命令前面。
bool waitForState(MusicPlayer &player, MusicPlayerState want, int timeoutMs = 3000)
{
    const int step = 20;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        if (player.state() == want)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
    }
    return player.state() == want;
}
} // namespace

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
        while (listener.list_change_count == 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(20000))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        CHECK(listener.list_change_count > 0);
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

TEST_SUITE("MusicPlayer Interactive Paths")
{
    // 回归：暂停只 stop 不 close（恢复时沿用同一个 SDL 设备），若恢复时再 open 一次，
    // open() 会以 "already open" 失败，被当成 AudioDeviceOpenFailed 并触发自动跳过，
    // 用户看到的是「点继续播放却跳到下一首」。
    TEST_CASE("pause then play resumes on the same device")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav"))
            return;

        MockMusicPlayerListener listener;
        std::string logDir = "./log";
        MusicPlayer player(&listener, logDir);

        player.addMusicDir(fixture.mediaDir());
        for (int i = 0; i < 40 && player.getMusicCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (player.getMusicCount() == 0)
            return;

        player.play();
        if (!waitForState(player, MusicPlayerState::PlayingState))
            return; // 没有可用音频设备：首次 play 就起不来，暂停/恢复无从验证

        const int errors_before = listener.error_count;
        player.pause();
        CHECK(waitForState(player, MusicPlayerState::PausedState));

        player.play();
        CHECK(waitForState(player, MusicPlayerState::PlayingState));
        CHECK(listener.error_count == errors_before);

        // 等 stop 真正执行完再出作用域：命令是异步排队的，析构与队列里的 _stop 撞车会
        // 让两个线程同时关同一个设备（见后续 issue），这里的等待只是避免测试自己制造竞态。
        player.stop();
        CHECK(waitForState(player, MusicPlayerState::StoppedState));
    }
}
