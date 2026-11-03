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

// 文件级错误的原因文案：措辞归 app 层（规格 8.3 只规定「必须分档、必须不同」）。
// 每个错误码一句，禁止共用——「可能不完整」与「已损坏」必须是两句不同的话。
QString errorReason(soundbridge::ErrorCode code)
{
    switch (code) {
    case soundbridge::ErrorCode::ExtractorUnsupported:
        return "FORMAT NOT SUPPORTED";
    case soundbridge::ErrorCode::ExtractorInitFailed:
        return "FILE IS DAMAGED - CANNOT PLAY";
    case soundbridge::ErrorCode::FileOpenFailed:
        return "FILE CANNOT BE OPENED";
    case soundbridge::ErrorCode::DecodeInitFailed:
        return "FILE CANNOT BE DECODED";
    case soundbridge::ErrorCode::DataCorrupted:
        return "FILE IS CORRUPTED";
    case soundbridge::ErrorCode::DataTruncated:
        return "FILE MAY BE INCOMPLETE";
    default:
        return QString::fromUtf8(soundbridge::GetErrorInfo(code).message).toUpper();
    }
}

// 提示文案按「原因 + 位置 + 处置」拼：处置来自 ErrorInfo.action / autoSkip（ADR 0001），
// 位置来自已播到的 curPositionMs（规格 8.3：必须带已播到的位置）。
QString errorSubtitle(soundbridge::ErrorCode code, bool autoSkip, qulonglong positionMs,
                      int trackIndex, const QString &detail)
{
    // 批量扫描汇总（不绑定曲目）：detail 就是「N 个文件无法播放」，直接显示计数
    if (trackIndex < 0 && !detail.isEmpty()) {
        return detail.toUpper();
    }

    const soundbridge::ErrorInfo &info = soundbridge::GetErrorInfo(code);
    QString subtitle                   = errorReason(code);
    if (info.recoverable && positionMs > 0) {
        subtitle += " AT " + formatTime(positionMs);
    }
    if (info.action == soundbridge::ErrorAction::SkipTrack) {
        subtitle += autoSkip ? " - SKIPPED" : " - STOPPED";
    }
    return subtitle;
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
    if (m_errorSubtitleActive) {
        // 错误提示要留到用户下一次操作或换曲，别被随后的 Playing/Paused 立刻覆盖
        publishViewState();
        return;
    }
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
    m_currentTrackIndex    = index;
    m_errorSubtitleActive  = false;
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
    const QString detailText = QString::fromUtf8(detail.c_str());
    m_viewState.subtitle
        = errorSubtitle(code, m_autoSkipOnError, m_viewState.positionMs, trackIndex, detailText);
    m_errorSubtitleActive = true;
    publishViewState();
    emit errorOccurred(static_cast<int>(code), detailText, trackIndex,
                       QString::fromUtf8(path.c_str()), QString::fromUtf8(traceId.c_str()),
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
