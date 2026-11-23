#pragma once

// 命令口与状态事实（ADR 0016）在 app/playerport.h；本类是生产实现。
#include "playerport.h"
#include "soundbridge/sdk.h"
#include <QObject>
#include <QStringList>
#include <memory>

class PlayerController : public QObject,
                        public soundbridge::PlayerCallbacks,
                        public PlayerCommandPort {
    Q_OBJECT

public:
    explicit PlayerController(const soundbridge::PlayerConfig &config, QObject *parent = nullptr);
    ~PlayerController() override;

    /* 错误副标题组装（UI-E3/E4/E5/E6 表驱动断言的测试缝，实现在 .cpp 匿名命名
       空间上移为静态方法；生产路径 onError 内部仍走同一份代码）。 */
    static QString errorReasonText(soundbridge::ErrorCode code);
    static QString composeErrorSubtitle(soundbridge::ErrorCode code, bool autoSkip,
                                        qulonglong positionMs, int trackIndex,
                                        const QString &detail);

    void addMusicDirectory(const std::string &path) override;
    void removeTrack(int index) override;
    void clearPlaylist() override;
    void playPause() override;
    void nextTrack() override;
    void previousTrack() override;
    void selectTrack(int index) override;
    void seekSeconds(int seconds) override;
    void setPlaybackMode(soundbridge::PlaybackMode mode) override;
    soundbridge::PlaybackMode playbackMode() const override;
    void setAutoSkipOnError(bool enabled) override;
    bool autoSkipOnError() const override;
    void setVolume(int percent) override;
    int volume() const override;
    void setMuted(bool muted) override;
    bool muted() const override;
    int trackCount() const override;
    int currentTrackIndex() const override;
    const PlayerViewState &viewState() const override;

signals:
    void viewStateChanged(PlayerViewState viewState);
    void errorOccurred(int code, const QString &detail, int trackIndex, const QString &path,
                       const QString &traceId, bool autoSkipEnabled);
    /* 换曲结果到达（onTrackChanged 的 Qt 信号半边，地图 #285 / #289）：切歌等待
       的撤回触发。app 层内部接缝——SDK 回调口（PlayerCallbacks）零改动。 */
    void trackSwitchSettled();

protected:
    void onStateChanged(soundbridge::PlayerState state) override;
    void onTrackChanged(int index) override;
    void onDurationChanged(uint64_t durationMs) override;
    void onPositionChanged(uint64_t positionMs) override;
    void onPlaylistChanged(const std::list<soundbridge::TrackInfo> &tracks) override;
    void onError(soundbridge::ErrorCode code, const std::string &detail, int trackIndex,
                 const std::string &path, const std::string &traceId) override;

private:
    void publishViewState();

private:
    std::unique_ptr<soundbridge::Player> m_player;
    soundbridge::PlaybackMode m_playbackMode = soundbridge::PlaybackMode::Sequential;
    bool m_autoSkipOnError                   = true;
    bool m_errorSubtitleActive               = false;
    int m_currentTrackIndex                  = -1;
    PlayerViewState m_viewState;
};
