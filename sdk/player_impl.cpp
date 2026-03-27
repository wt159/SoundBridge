#include "LogApi.h"
#include "MusicPlayer.h"
#include "soundbridge/player.h"

namespace soundbridge {

// Adapter to convert new callbacks to old listener
class PlayerCallbacksAdapter : public sdk::MusicPlayerListener {
public:
    explicit PlayerCallbacksAdapter(PlayerCallbacks *callbacks)
        : m_callbacks(callbacks)
    {
    }

    void onMusicPlayerStateChanged(sdk::MusicPlayerState state) override
    {
        if (m_callbacks) {
            PlayerState newState = PlayerState::Stopped;
            switch (state) {
            case sdk::MusicPlayerState::PlayingState:
                newState = PlayerState::Playing;
                break;
            case sdk::MusicPlayerState::PausedState:
                newState = PlayerState::Paused;
                break;
            case sdk::MusicPlayerState::StoppedState:
                newState = PlayerState::Stopped;
                break;
            }
            m_callbacks->onStateChanged(newState);
        }
    }

    void onMusicPlayerListCurrentIndexChanged(int index) override
    {
        if (m_callbacks) {
            m_callbacks->onTrackChanged(index);
        }
    }

    void onMusicPlayerDurationChanged(uint64_t duration) override
    {
        if (m_callbacks) {
            m_callbacks->onDurationChanged(duration);
        }
    }

    void onMusicPlayerPositionChanged(uint64_t position) override
    {
        if (m_callbacks) {
            m_callbacks->onPositionChanged(position);
        }
    }

    void onMusicPlayerMusicListChanged(std::list<sdk::MusicIndex> list) override
    {
        if (m_callbacks) {
            std::list<TrackInfo> tracks;
            for (const auto &item : list) {
                TrackInfo info;
                info.index = item.index;
                info.name  = item.name;
                tracks.push_back(info);
            }
            m_callbacks->onPlaylistChanged(tracks);
        }
    }

    void onMusicPlayerError(sdk::ErrorCode code, const std::string &detail, int index,
                            const std::string &path, const std::string &traceId) override
    {
        if (m_callbacks) {
            m_callbacks->onError(static_cast<ErrorCode>(code), detail, index, path);
        }
    }

private:
    PlayerCallbacks *m_callbacks;
};

class Player::Impl {
public:
    Impl(PlayerCallbacks *callbacks, const PlayerConfig &config)
        : m_adapter(callbacks)
        , m_logDir(config.logDirectory)
        , m_playbackMode(PlaybackMode::Sequential)
    {
        sdk::SdkLogConfig logConfig;
        logConfig.directory = config.logDirectory;
        sdk::InitializeLogging(logConfig);

        m_player = std::make_shared<sdk::MusicPlayer>(&m_adapter, m_logDir);
        m_player->setAutoSkipOnError(config.autoSkipOnError);
    }

    ~Impl() = default;

    void addMusicDirectory(const std::string &path) { m_player->addMusicDir(path); }

    void play() { m_player->play(); }

    void pause() { m_player->pause(); }

    void stop() { m_player->stop(); }

    void seek(uint64_t positionMs) { m_player->setPosition(positionMs); }

    void next() { m_player->next(); }

    void previous() { m_player->previous(); }

    void setCurrentTrack(int index) { m_player->setCurrentIndex(index); }

    int trackCount() const { return m_player->getMusicCount(); }

    PlayerState state() const
    {
        auto s = m_player->state();
        switch (s) {
        case sdk::MusicPlayerState::PlayingState:
            return PlayerState::Playing;
        case sdk::MusicPlayerState::PausedState:
            return PlayerState::Paused;
        case sdk::MusicPlayerState::StoppedState:
            return PlayerState::Stopped;
        }
        return PlayerState::Stopped;
    }

    uint64_t duration() const { return m_cachedDuration; }

    uint64_t position() const { return m_cachedPosition; }

    std::string currentTrackName() const { return m_cachedTrackName; }

    void setPlaybackMode(PlaybackMode mode)
    {
        m_playbackMode = mode;
        // TODO: Forward to internal implementation
    }

    PlaybackMode playbackMode() const { return m_playbackMode; }

    void setAutoSkipOnError(bool enabled) { m_player->setAutoSkipOnError(enabled); }

    bool autoSkipOnError() const { return m_player->autoSkipOnError(); }

private:
    PlayerCallbacksAdapter m_adapter;
    std::shared_ptr<sdk::MusicPlayer> m_player;
    std::string m_logDir;
    PlaybackMode m_playbackMode;
    uint64_t m_cachedDuration = 0;
    uint64_t m_cachedPosition = 0;
    std::string m_cachedTrackName;
};

Player::Player(PlayerCallbacks *callbacks, const PlayerConfig &config)
    : m_impl(new Impl(callbacks, config))
{
}

Player::~Player() = default;

void Player::addMusicDirectory(const std::string &path)
{
    m_impl->addMusicDirectory(path);
}

void Player::play()
{
    m_impl->play();
}

void Player::pause()
{
    m_impl->pause();
}

void Player::stop()
{
    m_impl->stop();
}

void Player::seek(uint64_t positionMs)
{
    m_impl->seek(positionMs);
}

void Player::next()
{
    m_impl->next();
}

void Player::previous()
{
    m_impl->previous();
}

void Player::setCurrentTrack(int index)
{
    m_impl->setCurrentTrack(index);
}

int Player::trackCount() const
{
    return m_impl->trackCount();
}

PlayerState Player::state() const
{
    return m_impl->state();
}

uint64_t Player::duration() const
{
    return m_impl->duration();
}

uint64_t Player::position() const
{
    return m_impl->position();
}

std::string Player::currentTrackName() const
{
    return m_impl->currentTrackName();
}

void Player::setPlaybackMode(PlaybackMode mode)
{
    m_impl->setPlaybackMode(mode);
}

PlaybackMode Player::playbackMode() const
{
    return m_impl->playbackMode();
}

void Player::setAutoSkipOnError(bool enabled)
{
    m_impl->setAutoSkipOnError(enabled);
}

bool Player::autoSkipOnError() const
{
    return m_impl->autoSkipOnError();
}

} // namespace soundbridge
