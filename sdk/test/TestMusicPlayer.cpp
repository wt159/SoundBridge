#include "LogWrapper.h"
#include "MusicPlayer.h"
#include "type_name.hpp"
#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>
using namespace std;
using namespace sdk;

#define LOG_TAG "TestMusicPlayer"

class TestMusicPlayer : public MusicPlayerListener {
private:
    std::shared_ptr<MusicPlayer> m_player;

public:
    TestMusicPlayer()
    {
        m_player        = std::make_shared<MusicPlayer>(this);
        std::string dir = "../../../music";
        m_player->addMusicDir(dir);
    }
    ~TestMusicPlayer() {
        m_player->stop();
    }

    void next()
    {
        m_player->next();
    }

    void play() {
        m_player->play();
    }

protected:
    virtual void onMusicPlayerStateChanged(MusicPlayerState state)
    {
        // LOG_INFO(LOG_TAG, "onMusicPlayerStateChanged : %s", type_name<decltype(state)>().data());
    }
    virtual void onMusicPlayerListCurrentIndexChanged(int index)
    {
        LOG_INFO(LOG_TAG, "onMusicPlayerListCurrentIndexChanged : %d", index);
    }
    virtual void onMusicPlayerDurationChanged(uint64_t duration)
    {
        LOG_INFO(LOG_TAG, "onMusicPlayerDurationChanged : %llu", duration);
    }
    virtual void onMusicPlayerPositionChanged(uint64_t position)
    {
        LOG_INFO(LOG_TAG, "onMusicPlayerPositionChanged : %llu", position);
    }
};

void TestCode()
{
    TestMusicPlayer test;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    test.play();
    std::this_thread::sleep_for(std::chrono::seconds(30));
}

int main()
{
    std::string rotateFileLog  = "music_player_test";
    std::string directory      = "./log";
    constexpr int k10MBInBytes = 10 * 1024 * 1024;
    constexpr int k20InCounts  = 20;
    printf("LogWrapper::getInstanceInitialize\n");
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10MBInBytes, k20InCounts);
    LOG_INFO(LOG_TAG, "Log init success");

    TestCode();

    return 0;
}