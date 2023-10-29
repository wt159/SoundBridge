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
        std::string directory      = "./log";
        m_player        = std::make_shared<MusicPlayer>(this, directory);
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
    virtual void onMusicPlayerMusicListChanged(std::list<MusicIndex> list)
    {
        LOG_INFO(LOG_TAG, "onMusicPlayerMusicListChanged : %d", list.size());
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
    TestCode();

    return 0;
}