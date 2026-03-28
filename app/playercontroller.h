#pragma once

#include "soundbridge/sdk.h"
#include <QObject>
#include <QStringList>
#include <memory>

struct PlayerViewState {
    int playerState       = static_cast<int>(soundbridge::PlayerState::Stopped);
    int playbackMode      = static_cast<int>(soundbridge::PlaybackMode::Sequential);
    bool autoSkipOnError  = true;
    int currentTrackIndex = -1;
    qulonglong durationMs = 0;
    qulonglong positionMs = 0;
    QStringList playlist;
    QString title = "—";
    QString artist;
    QString album;
    QString formattedDuration = "00:00";
    QString formattedPosition = "00:00";
    QString subtitle          = "IDLE";
};

Q_DECLARE_METATYPE(PlayerViewState)

class PlayerController : public QObject, public soundbridge::PlayerCallbacks {
    Q_OBJECT

public:
    explicit PlayerController(const soundbridge::PlayerConfig &config, QObject *parent = nullptr);
    ~PlayerController() override;

    void addMusicDirectory(const std::string &path);
    void playPause();
    void nextTrack();
    void previousTrack();
    void selectTrack(int index);
    void seekSeconds(int seconds);
    void setPlaybackMode(soundbridge::PlaybackMode mode);
    soundbridge::PlaybackMode playbackMode() const;
    void setAutoSkipOnError(bool enabled);
    bool autoSkipOnError() const;
    int trackCount() const;
    int currentTrackIndex() const;
    const PlayerViewState &viewState() const;

signals:
    void viewStateChanged(PlayerViewState viewState);
    void errorOccurred(int code, const QString &detail, int trackIndex, const QString &path,
                       const QString &traceId, bool autoSkipEnabled);

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
    int m_currentTrackIndex                  = -1;
    PlayerViewState m_viewState;
};
