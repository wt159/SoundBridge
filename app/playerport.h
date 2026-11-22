#pragma once

// app 层 UI 自动化的双注入缝（ADR 0016，地图 #248 / 执行票 #257）。
//
// 本头文件是接缝的唯一声明点：
// 1. `PlayerViewState`：三层真相源的唯一状态事实（ADR 0012），从 playercontroller.h
//    上移至此——命令口与渲染入口都以它为载体，放在一起避免循环包含。
// 2. `PlayerCommandPort`：纯虚命令口，17 个公开方法与 `PlayerController` 同名同签名
//    （app/playercontroller.h）。`MainWindow` 只依赖本接口；生产实现是
//    `PlayerController`，测试注入记录型 fake（QObject + 同签名信号，
//    复用既有 `SLOT()` 字符串连接）。
// 3. `AppUiPorts`：构造注入参数。三字段全空缺省 = 生产现状（自建 `PlayerController`、
//    曲库取 `applicationDirPath()/../../music`）。
//    `playerSignals` 与 `player` 指向同一对象：接口本身不是 QObject（不能让
//    `PlayerController` 出现第二个 QObject 子对象），信号连接需要 QObject 面，
//    故由注入方显式给出；生产路径由 `mediaPlayerInit()` 自填。
//
// 不变量（ADR 0016）：`soundbridge::Player` 公开面零改动；注入路径不构造
// `PlayerController`，测试进程零 SDL。

#include <QMetaType>
#include <QString>
#include <QStringList>

#include "soundbridge/sdk.h"

/* 唯一状态事实（ADR 0012）。原声明在 playercontroller.h，随接缝上移。 */
struct PlayerViewState {
    int playerState       = static_cast<int>(soundbridge::PlayerState::Stopped);
    int playbackMode      = static_cast<int>(soundbridge::PlaybackMode::Sequential);
    bool autoSkipOnError  = true;
    int currentTrackIndex = -1;
    qulonglong durationMs = 0;
    qulonglong positionMs = 0;
    QStringList playlist;
    // 与 playlist 同序的派生身份键（ADR 0014），来自 TrackInfo::id，可为空串。
    QStringList trackIds;
    QString title = "—";
    QString artist;
    QString album;
    QString formattedDuration = "00:00";
    QString formattedPosition = "00:00";
    QString subtitle          = "IDLE";
};

Q_DECLARE_METATYPE(PlayerViewState)

/* 命令口（ADR 0016）：17 个方法与 PlayerController 公开面逐一对应。 */
class PlayerCommandPort {
public:
    virtual ~PlayerCommandPort() = default;

    virtual void addMusicDirectory(const std::string &path) = 0;
    virtual void playPause()                                = 0;
    virtual void nextTrack()                                = 0;
    virtual void previousTrack()                            = 0;
    virtual void selectTrack(int index)                     = 0;
    virtual void seekSeconds(int seconds)                   = 0;
    virtual void setPlaybackMode(soundbridge::PlaybackMode mode) = 0;
    virtual soundbridge::PlaybackMode playbackMode() const       = 0;
    virtual void setAutoSkipOnError(bool enabled)                = 0;
    virtual bool autoSkipOnError() const                         = 0;
    virtual void setVolume(int percent)                          = 0;
    virtual int volume() const                                   = 0;
    virtual void setMuted(bool muted)                            = 0;
    virtual bool muted() const                                   = 0;
    virtual int trackCount() const                               = 0;
    virtual int currentTrackIndex() const                        = 0;
    virtual const PlayerViewState &viewState() const             = 0;
};

/* 构造注入参数（ADR 0016 / ADR 0017）：缺省 = 生产现状。 */
struct AppUiPorts {
    /* 命令口替身；nullptr ⇒ 自建 PlayerController（生产路径）。 */
    PlayerCommandPort *player = nullptr;
    /* 同一对象的 QObject 面（信号源），供 `SLOT()` 字符串连接复用；生产自填。 */
    QObject *playerSignals = nullptr;
    /* 曲库根；空 ⇒ applicationDirPath() + "/../../music"（现状）。 */
    QString musicDirectory;
};
