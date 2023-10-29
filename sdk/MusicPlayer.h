/**
 * @file MusicPlayer.h
 * @author wtp (wtp0727@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-08-27
 *
 * @copyright Copyright (c) 2023
 *
 */

#pragma once
#include <cstdint>
#include <memory>
#include <list>

namespace sdk {

enum class MusicPlayerState {
    PlayingState,
    PausedState,
    StoppedState,
};

enum class MusicPlaybackMode {
    CurrentItemOnce,
    CurrentItemInLoop,
    Sequential,
    Loop,
    Random,
};

struct MusicIndex {
    int index;
    std::string name;
};

class MusicPlayerListener {
public:
    virtual ~MusicPlayerListener()                                    = default;
    virtual void onMusicPlayerStateChanged(MusicPlayerState)          = 0;
    virtual void onMusicPlayerListCurrentIndexChanged(int)            = 0;
    virtual void onMusicPlayerDurationChanged(uint64_t)               = 0;
    virtual void onMusicPlayerPositionChanged(uint64_t)               = 0;
    virtual void onMusicPlayerMusicListChanged(std::list<MusicIndex>) = 0;
};

class MusicPlayer {
public:
    MusicPlayer()                               = delete;
    MusicPlayer(MusicPlayer &)                  = delete;
    MusicPlayer(MusicPlayer &&)                 = delete;
    MusicPlayer &operator=(const MusicPlayer &) = delete;
    MusicPlayer &operator=(MusicPlayer &&)      = delete;

public:
    MusicPlayer(MusicPlayerListener *listener, std::string &log_dir);
    ~MusicPlayer();

    // player
    void addMusicDir(const std::string &dir);
    void play();
    void pause();
    void stop();
    MusicPlayerState state();
    void setPosition(uint64_t pos);

    // list
    void next();
    void previous();
    void setCurrentIndex(int index);
    int getMusicCount();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sdk