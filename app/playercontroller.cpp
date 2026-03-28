#include "playercontroller.h"

namespace {
QString formatTime(qulonglong timeMs)
{
    int second  = static_cast<int>(timeMs / 1000);
    int minute  = second / 60;
    second     %= 60;

    QString formatted
        = (minute >= 10 ? QString::number(minute, 10) : "0" + QString::number(minute, 10));
    formatted += (second >= 10 ? ":" : ":0") + QString::number(second, 10);
    return formatted;
}
}

PlayerController::PlayerController(const soundbridge::PlayerConfig &config, QObject *parent)
    : QObject(parent)
    , m_player(new soundbridge::Player(this, config))
    , m_playbackMode(soundbridge::PlaybackMode::Sequential)
    , m_autoSkipOnError(config.autoSkipOnError)
{
    qRegisterMetaType<PlayerViewState>("PlayerViewState");
    m_player->setAutoSkipOnError(config.autoSkipOnError);
    m_viewState.autoSkipOnError = config.autoSkipOnError;
}

PlayerController::~PlayerController() = default;

void PlayerController::addMusicDirectory(const std::string &path)
{
    m_player->addMusicDirectory(path);
}

void PlayerController::playPause()
{
    switch (m_player->state()) {
    case soundbridge::PlayerState::Stopped:
    case soundbridge::PlayerState::Paused:
        m_player->play();
        break;
    case soundbridge::PlayerState::Playing:
        m_player->pause();
        break;
    }
}

void PlayerController::nextTrack()
{
    if (m_player->trackCount() == 0) {
        return;
    }
    m_player->stop();
    m_player->next();
    m_player->play();
}

void PlayerController::previousTrack()
{
    if (m_player->trackCount() == 0) {
        return;
    }
    m_player->stop();
    m_player->previous();
    m_player->play();
}

void PlayerController::selectTrack(int index)
{
    if (index < 0 || index >= m_player->trackCount()) {
        return;
    }
    m_player->stop();
    m_player->setCurrentTrack(index);
    m_player->play();
}

void PlayerController::seekSeconds(int seconds)
{
    if (seconds < 0) {
        return;
    }
    m_player->seek(static_cast<uint64_t>(seconds) * 1000);
}

void PlayerController::setPlaybackMode(soundbridge::PlaybackMode mode)
{
    m_playbackMode = mode;
    m_player->setPlaybackMode(mode);
    publishViewState();
}

soundbridge::PlaybackMode PlayerController::playbackMode() const
{
    return m_playbackMode;
}

void PlayerController::setAutoSkipOnError(bool enabled)
{
    m_autoSkipOnError = enabled;
    m_player->setAutoSkipOnError(enabled);
    publishViewState();
}

bool PlayerController::autoSkipOnError() const
{
    return m_autoSkipOnError;
}

int PlayerController::trackCount() const
{
    return m_player->trackCount();
}

int PlayerController::currentTrackIndex() const
{
    return m_currentTrackIndex;
}

const PlayerViewState &PlayerController::viewState() const
{
    return m_viewState;
}

void PlayerController::onStateChanged(soundbridge::PlayerState state)
{
    m_viewState.playerState = static_cast<int>(state);
    switch (state) {
    case soundbridge::PlayerState::Stopped:
        m_viewState.subtitle = "IDLE";
        break;
    case soundbridge::PlayerState::Playing:
    case soundbridge::PlayerState::Paused:
        m_viewState.subtitle = "NOW PLAYING";
        break;
    }
    publishViewState();
}

void PlayerController::onTrackChanged(int index)
{
    m_currentTrackIndex = index;
    publishViewState();
}

void PlayerController::onDurationChanged(uint64_t durationMs)
{
    m_viewState.durationMs = static_cast<qulonglong>(durationMs);
    publishViewState();
}

void PlayerController::onPositionChanged(uint64_t positionMs)
{
    m_viewState.positionMs = static_cast<qulonglong>(positionMs);
    publishViewState();
}

void PlayerController::onPlaylistChanged(const std::list<soundbridge::TrackInfo> &tracks)
{
    QStringList names;
    for (const auto &track : tracks) {
        names.push_back(QString::fromUtf8(track.name.c_str()));
    }
    m_viewState.playlist = names;
    publishViewState();
}

void PlayerController::onError(soundbridge::ErrorCode code, const std::string &detail,
                               int trackIndex, const std::string &path, const std::string &traceId)
{
    m_viewState.subtitle = m_autoSkipOnError ? "SKIPPING ERROR" : "PLAYBACK ERROR";
    publishViewState();
    emit errorOccurred(static_cast<int>(code), QString::fromStdString(detail), trackIndex,
                       QString::fromStdString(path), QString::fromStdString(traceId),
                       m_autoSkipOnError);
}

void PlayerController::publishViewState()
{
    m_viewState.playbackMode      = static_cast<int>(m_playbackMode);
    m_viewState.autoSkipOnError   = m_autoSkipOnError;
    m_viewState.currentTrackIndex = m_currentTrackIndex;
    m_viewState.formattedDuration = formatTime(m_viewState.durationMs);
    m_viewState.formattedPosition = formatTime(m_viewState.positionMs);
    if (m_currentTrackIndex >= 0 && m_currentTrackIndex < m_viewState.playlist.size()) {
        m_viewState.title = m_viewState.playlist.at(m_currentTrackIndex);
    } else {
        m_viewState.title = "—";
    }
    emit viewStateChanged(m_viewState);
}
