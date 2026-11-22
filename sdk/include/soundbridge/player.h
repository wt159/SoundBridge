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
    // 派生身份键：由 fullPath 归一化而来（绝对化 + 词法归一 + 分隔符统一 '/' +
    // Windows 大小写折叠），跨扫描稳定，用于收藏等跨会话标注（ADR 0014）。
    // 允许为空：没有路径的来源（将来的流式条目）为空。**不保证列表内唯一**——
    // addMusic 无去重，同一路径入列两次会得到两条同 id 记录，不得当唯一键做索引。
    std::string id;
};

struct PlayerConfig {
    std::string logDirectory;
    bool autoSkipOnError = true;
};

// 回调在 SDK 内部单一串行线程上同步发出，不在调用者线程、不并发。
// 调用方自行 marshal 到自己的线程。回调内禁止调用 Player 的任何控制命令，
// 禁止析构 Player，禁止抛异常。callbacks 由调用方持有，须活过 Player 析构；
// 停机期间不保证也不发送任何回调。
//
// 完整契约（状态取值、回调顺序、控制命令语义、错误先后）见
// doc/player-state-contract.md。
class PlayerCallbacks {
public:
    virtual ~PlayerCallbacks()                                         = default;
    virtual void onStateChanged(PlayerState state)                     = 0;
    virtual void onTrackChanged(int index)                             = 0;
    virtual void onDurationChanged(uint64_t durationMs)                = 0;
    virtual void onPositionChanged(uint64_t positionMs)                = 0;
    virtual void onPlaylistChanged(const std::list<TrackInfo> &tracks) = 0;
    virtual void onError(ErrorCode code, const std::string &detail, int trackIndex,
                         const std::string &path, const std::string &traceId)
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

    // Playlist editing（ADR 0018，契约见 doc/player-state-contract.md §4.1）
    // 三条命令都是 `void` + 纯入队：结果经回调与查询可见，调用者线程不做文件系统访问。
    // - addTrack：单个文件（传目录丢弃）；允许重复路径；只在列表原本为空时设为当前曲目，
    //   不自动播放。失败发一次 onError(index == -1)，列表不变。
    // - removeTrack：下标按「执行时刻」解释；越界丢弃。删的是当前曲目时按播放模式补位
    //   （列表非空就一定有当前曲目），Playing 保持 Playing，变空则停止。
    // - clearPlaylist：空列表上为幂等丢弃；非空则变为「无当前曲目」，Playing/Paused → Stopped。
    // 回调顺序：onPlaylistChanged 先于同一命令引发的 onTrackChanged；当前曲目未换、
    // 仅其 index 因压缩重排而改变时，也会补发一次 onTrackChanged(新下标)。
    void addTrack(const std::string &path);
    void removeTrack(int index);
    void clearPlaylist();

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

    // 音量（软件增益，ADR 0013）：0–100 整数，100 = 增益 1.0 原样输出，
    // 0 = 输出全零 PCM；越界按端点钳制，不报错。不支持 >100（不做软件放大）。
    // 静音是与取值正交的独立布尔：静音不改 volume()，取消静音回到原值。
    // 生效有排队延迟——稳态设备队列约 256 KiB，改动在已排队 PCM 播完后才可听
    // （≈1.5 s 量级），不承诺毫秒级上限。跨曲目 / stop-play / 设备重开保持。
    void setVolume(int percent);
    int volume() const;
    void setMuted(bool muted);
    bool muted() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace soundbridge
