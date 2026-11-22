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
#include "ErrorCode.hpp"
#include "PlaybackMode.h"
#include "StreamMetrics.h"
#include <cstdint>
#include <list>
#include <memory>
#include <string>

class IAudioDevice;

namespace sdk {

enum class MusicPlayerState {
    PlayingState,
    PausedState,
    StoppedState,
};

struct MusicIndex {
    int index;
    std::string name;
    // 派生身份键（ADR 0014）：与 soundbridge::TrackInfo::id 同源，允许为空。
    std::string id;
};

class MusicPlayerListener {
public:
    virtual ~MusicPlayerListener()                                    = default;
    virtual void onMusicPlayerStateChanged(MusicPlayerState)          = 0;
    virtual void onMusicPlayerListCurrentIndexChanged(int)            = 0;
    virtual void onMusicPlayerDurationChanged(uint64_t)               = 0;
    virtual void onMusicPlayerPositionChanged(uint64_t)               = 0;
    virtual void onMusicPlayerMusicListChanged(std::list<MusicIndex>) = 0;
    virtual void onMusicPlayerError(ErrorCode code, const std::string &detail, int index,
                                    const std::string &path, const std::string &traceId)
        = 0;
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
    // 列表编辑（ADR 0018 / 契约 §4.1）：三条命令都是纯入队，调用者线程零文件系统访问。
    // 结果经既有回调（onMusicPlayerMusicListChanged / ...CurrentIndexChanged / Error）可见。
    void addTrack(const std::string &path);
    void removeTrack(int index);
    void clearPlaylist();
    void setPlaybackMode(MusicPlaybackMode mode);
    MusicPlaybackMode playbackMode() const;
    void setAutoSkipOnError(bool enabled);
    bool autoSkipOnError() const;

    // 音量（软件增益，ADR 0013）：0–100，100 = 原样输出；静音与取值正交。
    void setVolume(int percent);
    int volume() const;
    void setMuted(bool muted);
    bool muted() const;

    // 仅供测试（ADR 0010）：设备队列当前深度（字节）。不进入 soundbridge::Player 的对外契约。
    size_t queuedBytesForTest() const;
    // 仅供测试（ADR 0010）：当前播放任务的解码器仪表快照；无任务时返回零值。
    StreamMetrics metricsForTest() const;
    // 仅供测试（ADR 0011）：退役槽是否已腾空（`retiredTaskForTest()` 过期）。用来观察
    // 消费拍里的 `reapRetiredTaskIfFinished()`：只读，不构成第二条回收路径。
    bool retiredTaskExpiredForTest() const;
    // 仅供测试（ADR 0011）：在 play() 前替换音频设备。设备已 open 时拒绝并返回 false，
    // 保持原指针（同进程只允许一个输出设备，替换已 open 的设备会让它无人 close）。
    bool setAudioDeviceForTest(std::shared_ptr<IAudioDevice> device);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace sdk
