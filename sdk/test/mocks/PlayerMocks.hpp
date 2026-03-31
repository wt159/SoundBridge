#pragma once

#include "../../MusicPlayer.h"
#include "../include/soundbridge/player.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>

class MockPlayerCallbacks : public soundbridge::PlayerCallbacks {
public:
    std::atomic<int> state_change_count { 0 };
    std::atomic<int> track_change_count { 0 };
    std::atomic<int> duration_change_count { 0 };
    std::atomic<int> position_change_count { 0 };
    std::atomic<int> playlist_change_count { 0 };
    std::atomic<int> error_count { 0 };
    std::atomic<int> last_track_index { -1 };
    std::atomic<uint64_t> last_duration { 0 };
    std::atomic<uint64_t> last_position { 0 };

    mutable std::mutex mutex;
    std::string last_trace_id;
    std::map<int, std::string> track_names;
    soundbridge::PlayerState last_state    = soundbridge::PlayerState::Stopped;
    soundbridge::ErrorCode last_error_code = soundbridge::ErrorCode::Unknown;
    std::string last_error_detail;
    int last_error_track_index = -1;
    std::string last_error_path;

    void reset()
    {
        state_change_count    = 0;
        track_change_count    = 0;
        duration_change_count = 0;
        position_change_count = 0;
        playlist_change_count = 0;
        error_count           = 0;
        last_track_index      = -1;
        last_duration         = 0;
        last_position         = 0;
        last_trace_id.clear();
        track_names.clear();
        last_state = soundbridge::PlayerState::Stopped;
    }

    void onStateChanged(soundbridge::PlayerState state) override
    {
        state_change_count.fetch_add(1);
        last_state = state;
    }

    void onTrackChanged(int index) override
    {
        track_change_count.fetch_add(1);
        last_track_index.store(index);
    }

    void onDurationChanged(uint64_t durationMs) override
    {
        duration_change_count.fetch_add(1);
        last_duration.store(durationMs);
    }

    void onPositionChanged(uint64_t positionMs) override
    {
        position_change_count.fetch_add(1);
        last_position.store(positionMs);
    }

    void onPlaylistChanged(const std::list<soundbridge::TrackInfo> &tracks) override
    {
        playlist_change_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        track_names.clear();
        for (const auto &track : tracks) {
            track_names[track.index] = track.name;
        }
    }

    void onError(soundbridge::ErrorCode code, const std::string &detail, int trackIndex,
                 const std::string &path, const std::string &traceId) override
    {
        error_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        last_error_code        = code;
        last_error_detail      = detail;
        last_error_track_index = trackIndex;
        last_error_path        = path;
        last_trace_id          = traceId;
    }

    bool hasTrack(int index) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return track_names.find(index) != track_names.end();
    }

    std::string getTrackName(int index) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = track_names.find(index);
        return (it != track_names.end()) ? it->second : std::string();
    }
};

class MockMusicPlayerListener : public sdk::MusicPlayerListener {
public:
    std::atomic<int> state_change_count { 0 };
    std::atomic<int> index_change_count { 0 };
    std::atomic<int> duration_change_count { 0 };
    std::atomic<int> position_change_count { 0 };
    std::atomic<int> list_change_count { 0 };
    std::atomic<int> error_count { 0 };
    std::atomic<int> last_current_index { -1 };
    std::atomic<uint64_t> last_duration { 0 };
    std::atomic<uint64_t> last_position { 0 };

    mutable std::mutex mutex;
    std::string last_trace_id;
    std::map<int, std::string> music_list;
    sdk::MusicPlayerState last_state = sdk::MusicPlayerState::StoppedState;
    sdk::ErrorCode last_error_code   = sdk::ErrorCode::Unknown;
    std::string last_error_detail;
    int last_error_index = -1;
    std::string last_error_path;

    void reset()
    {
        state_change_count    = 0;
        index_change_count    = 0;
        duration_change_count = 0;
        position_change_count = 0;
        list_change_count     = 0;
        error_count           = 0;
        last_current_index    = -1;
        last_duration         = 0;
        last_position         = 0;
        last_trace_id.clear();
        music_list.clear();
        last_state = sdk::MusicPlayerState::StoppedState;
    }

    void onMusicPlayerStateChanged(sdk::MusicPlayerState state) override
    {
        state_change_count.fetch_add(1);
        last_state = state;
    }

    void onMusicPlayerListCurrentIndexChanged(int index) override
    {
        index_change_count.fetch_add(1);
        last_current_index.store(index);
    }

    void onMusicPlayerDurationChanged(uint64_t durationMs) override
    {
        duration_change_count.fetch_add(1);
        last_duration.store(durationMs);
    }

    void onMusicPlayerPositionChanged(uint64_t positionMs) override
    {
        position_change_count.fetch_add(1);
        last_position.store(positionMs);
    }

    void onMusicPlayerMusicListChanged(std::list<sdk::MusicIndex> tracks) override
    {
        list_change_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        music_list.clear();
        for (const auto &track : tracks) {
            music_list[track.index] = track.name;
        }
    }

    void onMusicPlayerError(sdk::ErrorCode code, const std::string &detail, int index,
                            const std::string &path, const std::string &traceId) override
    {
        error_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        last_error_code   = code;
        last_error_detail = detail;
        last_error_index  = index;
        last_error_path   = path;
        last_trace_id     = traceId;
    }

    bool hasTrack(int index) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return music_list.find(index) != music_list.end();
    }

    std::string getTrackName(int index) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = music_list.find(index);
        return (it != music_list.end()) ? it->second : std::string();
    }
};
