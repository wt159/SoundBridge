#include "AudioBuffer.h"
#include "AudioDevice.h"
#include "FileSearch.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "MusicPlayer.h"
#include "WorkQueue.hpp"

namespace sdk {

#define LOG_TAG "MusicPlayer"

class MusicPlayer::Impl : public AudioDataCallback, public MusicPlayListCallback {
public:
    Impl(MusicPlayerListener *listener);
    ~Impl() = default;
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

protected:
    virtual void getAudioData(void *data, int len);
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property);

private:
    AudioDevice m_audioDev;
    MusicPlayList m_musicList;
    MusicPlayerListener *m_listener;
    MusicPlayerState m_state;
    WorkQueue m_workQueue;
    MusicPropertiesPtr m_curMusicProperties;
};

MusicPlayer::Impl::Impl(MusicPlayerListener *lister)
    : m_audioDev(this)
    , m_musicList(this)
    , m_listener(lister)
    , m_state(MusicPlayerState::StoppedState)
    , m_workQueue()
{
    LOG_INFO(LOG_TAG, "Impl construct");
}

void MusicPlayer::Impl::addMusicDir(const std::string &dir)
{
    LOG_INFO(LOG_TAG, "addMusicDir : %s", dir.data());
    std::vector<std::string> musicList = recursiveFileSearch(dir);
    for (auto &path : musicList) {
        m_musicList.addMusic(path);
    }
}

void MusicPlayer::Impl::play()
{
    LOG_INFO(LOG_TAG, "play");
    if (m_curMusicProperties == nullptr)
        return;
    m_audioDev.open(m_curMusicProperties->signalProperties.spec);
    m_audioDev.start();
    m_state = MusicPlayerState::PlayingState;
}

void MusicPlayer::Impl::pause()
{
    LOG_INFO(LOG_TAG, "pause");
    if (m_curMusicProperties == nullptr)
        return;
    m_audioDev.stop();
    m_state = MusicPlayerState::PausedState;
}

void MusicPlayer::Impl::stop()
{
    LOG_INFO(LOG_TAG, "stop");
    if (m_curMusicProperties == nullptr)
        return;
    m_audioDev.stop();
    m_audioDev.close();
    m_state = MusicPlayerState::StoppedState;
}

MusicPlayerState MusicPlayer::Impl::state()
{
    return m_state;
}

void MusicPlayer::Impl::setPosition(uint64_t pos)
{
    LOG_INFO(LOG_TAG, "setPosition : %llu", pos);
    if (m_curMusicProperties && pos >= 0
        && pos <= m_curMusicProperties->signalProperties.durationMs) {
        m_curMusicProperties->signalProperties.curPositionMs = pos;
        m_curMusicProperties->signalProperties.curDataOffset = pos
            * m_curMusicProperties->signalProperties.spec.sampleRate
            * m_curMusicProperties->signalProperties.spec.bytesPerSample
            * m_curMusicProperties->signalProperties.spec.numChannel / 1000;
    }
}

void MusicPlayer::Impl::next()
{
    if (m_curMusicProperties == nullptr)
        return;
    stop();
    m_musicList.next();
    play();
}

void MusicPlayer::Impl::previous()
{
    if (m_curMusicProperties == nullptr)
        return;
    stop();
    m_musicList.pervious();
    play();
}

void MusicPlayer::Impl::setCurrentIndex(int index)
{
    if (m_curMusicProperties == nullptr)
        return;
    stop();
    m_musicList.setCurrentIndex(index);
    play();
}

int MusicPlayer::Impl::getMusicCount()
{
    return m_musicList.getMusicCount();
}

void MusicPlayer::Impl::getAudioData(void *data, int len)
{
    if (m_curMusicProperties) {
        SignalProperties &signalProperties = m_curMusicProperties->signalProperties;
        m_curMusicProperties->rawBuffer->getData(signalProperties.curDataOffset, len, (char *)data);
        AudioSpec &spec                 = signalProperties.spec;
        signalProperties.curDataOffset += len;
        signalProperties.curPositionMs
            += 1000LL * len / (spec.bytesPerSample * spec.numChannel) / spec.sampleRate;
        uint64_t curPositionMs = signalProperties.curPositionMs;
        m_workQueue.asyncRunTask(
            [this, curPositionMs]() { m_listener->onMusicPlayerPositionChanged(curPositionMs); });
        // LOG_DEBUG(LOG_TAG, "curDataOffset : %lld, len:%", signalProperties.curDataOffset.load());
        if (signalProperties.curDataOffset >= signalProperties.dataSize) {
            // m_musicList.next();
            signalProperties.curDataOffset = 0;
            signalProperties.curPositionMs = 0;
            stop();
        }

        if (m_state == MusicPlayerState::StoppedState) {
            m_state = MusicPlayerState::PlayingState;
            m_listener->onMusicPlayerStateChanged(m_state);
        }
    }
}

void MusicPlayer::Impl::putMusicPlayListCurBuf(MusicPropertiesPtr property)
{
    LOG_INFO(LOG_TAG, "putMusicPlayListCurBuf : %d", property->index);
    if (m_curMusicProperties
        && (m_curMusicProperties->signalProperties.spec.sampleRate
                != property->signalProperties.spec.sampleRate
            || m_curMusicProperties->signalProperties.spec.numChannel
                != property->signalProperties.spec.numChannel
            || m_curMusicProperties->signalProperties.spec.bytesPerSample
                != property->signalProperties.spec.bytesPerSample)) {
        LOG_DEBUG(LOG_TAG, "sampleRate : %d, numChannel : %d, bytesPerSample : %d",
                  property->signalProperties.spec.sampleRate,
                  property->signalProperties.spec.numChannel,
                  property->signalProperties.spec.bytesPerSample);
        stop();
        m_curMusicProperties = property;
        play();
    } else {
        m_curMusicProperties = property;
    }
    m_workQueue.asyncRunTask([this, property]() {
        m_listener->onMusicPlayerListCurrentIndexChanged(property->index);
        m_listener->onMusicPlayerDurationChanged(property->signalProperties.durationMs);
        m_listener->onMusicPlayerPositionChanged(property->signalProperties.curPositionMs);
    });
}

MusicPlayer::MusicPlayer(MusicPlayerListener *listener)
    : m_impl(new Impl(listener))
{
}

MusicPlayer::~MusicPlayer() { }

void MusicPlayer::addMusicDir(const std::string &dir)
{
    m_impl->addMusicDir(dir);
}

void MusicPlayer::play()
{
    m_impl->play();
}

void MusicPlayer::pause()
{
    m_impl->pause();
}

void MusicPlayer::stop()
{
    m_impl->stop();
}

MusicPlayerState MusicPlayer::state()
{
    return m_impl->state();
}

void MusicPlayer::setPosition(uint64_t pos)
{
    m_impl->setPosition(pos);
}

void MusicPlayer::next()
{
    m_impl->next();
}

void MusicPlayer::previous()
{
    m_impl->previous();
}

void MusicPlayer::setCurrentIndex(int index)
{
    m_impl->setCurrentIndex(index);
}

int MusicPlayer::getMusicCount()
{
    return m_impl->getMusicCount();
}
}
