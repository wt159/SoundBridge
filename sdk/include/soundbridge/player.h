#pragma once

#include "error.h"
#include <cstdint>
#include <list>
#include <memory>
#include <string>

namespace soundbridge {

enum class PlayerState { Playing, Paused, Stopped };

enum class PlaybackMode { Sequential, Loop, Random, SingleOnce, SingleLoop };

struct TrackInfo {
    int index;
    std::string name;
};

struct PlayerConfig {
    std::string logDirectory;
    bool autoSkipOnError = true;
};

class PlayerCallbacks {
public:
    virtual ~PlayerCallbacks()                                         = default;
    virtual void onStateChanged(PlayerState state)                     = 0;
    virtual void onTrackChanged(int index)                             = 0;
    virtual void onDurationChanged(uint64_t durationMs)                = 0;
    virtual void onPositionChanged(uint64_t positionMs)                = 0;
    virtual void onPlaylistChanged(const std::list<TrackInfo> &tracks) = 0;
    virtual void onError(ErrorCode code, const std::string &detail, int trackIndex,
                         const std::string &path)
        = 0;
};

class Player {
public:
    explicit Player(PlayerCallbacks *callbacks, const PlayerConfig &config);
    ~Player();

    Player(const Player &)            = delete;
    Player &operator=(const Player &) = delete;

    // Playback control
    void addMusicDirectory(const std::string &path);
    void play();
    void pause();
    void stop();
    void seek(uint64_t positionMs);

    // Playlist navigation
    void next();
    void previous();
    void setCurrentTrack(int index);
    int trackCount() const;

    // State queries
    PlayerState state() const;
    uint64_t duration() const;
    uint64_t position() const;
    std::string currentTrackName() const;

    // Settings
    void setPlaybackMode(PlaybackMode mode);
    PlaybackMode playbackMode() const;
    void setAutoSkipOnError(bool enabled);
    bool autoSkipOnError() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace soundbridge
