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

class MusicPlayerListener {
    virtual ~MusicPlayerListener() = default;
    virtual void onMusicPlayerStateChanged(MusicPlayerState) = 0;
    virtual void onMusicPlayerListCurrentIndexChanged(int) = 0;
    virtual void onMusicPlayerDurationChanged(uint64_t) = 0;
    virtual void onMusicPlayerPositionChanged(uint64_t) = 0;
};

class MusicPlayer {
public:
    MusicPlayer() = delete;
    MusicPlayer(MusicPlayer &) = delete;
    MusicPlayer(MusicPlayer &&) = delete;
    MusicPlayer &operator=(const MusicPlayer &) = delete;
    MusicPlayer &operator=(MusicPlayer &&) = delete;

public:
    MusicPlayer(MusicPlayerListener *listener);
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
    class MusicPlayerImpl;
    std::unique_ptr<MusicPlayerImpl> m_impl;
};
} // namespace wtp_sdk