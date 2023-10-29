#include "AudioBuffer.h"
#include "AudioDevice.h"
#include "FileSearch.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "MusicPlayer.h"
#include "WorkQueue.hpp"
#include <list>

namespace sdk {

#define LOG_TAG "MusicPlayer"

class MusicPlayer::Impl : public AudioDataCallback, public MusicPlayListCallback {
public:
    Impl(MusicPlayerListener *listener, std::string &logDir);
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
    void _addMusic(const std::string &musicPath);
    void _play();
    void _pause();
    void _stop();
    void _setPosition(uint64_t pos);
    void _next();
    void _previous();
    void _setCurrentIndex(int index);

protected:
    virtual void getAudioData(void *data, int len);
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property);
    virtual void updateMusicList(std::vector<MusicPropertiesPtr> &list);

private:
    void updatePlayState(MusicPlayerState state);

private:
    WorkQueue m_workQueue;
    std::shared_ptr<AudioDevice> m_audioDev;
    AudioSpec m_devSpec;
    std::shared_ptr<MusicPlayList> m_musicList;
    MusicPlayerListener *m_listener;
    MusicPlayerState m_state;
    MusicPropertiesPtr m_curMusicProperties;
    std::list<MusicIndex> m_musicListIndex;
};

MusicPlayer::Impl::Impl(MusicPlayerListener *lister, std::string &logDir)
    : m_workQueue()
    , m_audioDev(nullptr)
    , m_musicList(nullptr)
    , m_listener(lister)
    , m_state(MusicPlayerState::StoppedState)
{
    std::string rotateFileLog  = "music_player";
    std::string directory      = logDir;
    constexpr int k10MBInBytes = 10 * 1024 * 1024;
    constexpr int k20InCounts  = 20;
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10MBInBytes, k20InCounts);
    LOG_INFO(LOG_TAG, "Log init success");

    m_audioDev = std::make_shared<AudioDevice>(this);
    m_audioDev->getDeviceSpec(m_devSpec);
    m_musicList = std::make_shared<MusicPlayList>(this, &m_workQueue, m_devSpec);
    LOG_INFO(LOG_TAG, "Impl construct");
}

void MusicPlayer::Impl::addMusicDir(const std::string &dir)
{
    LOG_INFO(LOG_TAG, "addMusicDir : %s", dir.data());
    std::vector<std::string> musicList = recursiveFileSearch(dir);
    for (auto &path : musicList) {
        m_musicList->addMusic(path);
    }
    m_musicList->updateList();
}

void MusicPlayer::Impl::play()
{
    LOG_INFO(LOG_TAG, "play");

    m_workQueue.asyncRunTask([this]() { _play(); });
}

void MusicPlayer::Impl::pause()
{
    LOG_INFO(LOG_TAG, "pause");

    m_workQueue.asyncRunTask([this]() { _pause(); });
}

void MusicPlayer::Impl::stop()
{
    LOG_INFO(LOG_TAG, "stop");

    m_workQueue.asyncRunTask([this]() { _stop(); });
}

MusicPlayerState MusicPlayer::Impl::state()
{
    return m_state;
}

void MusicPlayer::Impl::setPosition(uint64_t pos)
{
    LOG_INFO(LOG_TAG, "setPosition : %llu", pos);
    m_workQueue.asyncRunTask([this, pos]() { _setPosition(pos); });
}

void MusicPlayer::Impl::next()
{
    m_workQueue.asyncRunTask([this]() { _next(); });
}

void MusicPlayer::Impl::previous()
{
    m_workQueue.asyncRunTask([this]() { _previous(); });
}

void MusicPlayer::Impl::setCurrentIndex(int index)
{
    m_workQueue.asyncRunTask([this, index]() { _setCurrentIndex(index); });
}

int MusicPlayer::Impl::getMusicCount()
{
    return m_musicList->getMusicCount();
}

void MusicPlayer::Impl::_addMusic(const std::string &musicPath) { }

void MusicPlayer::Impl::_play()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_audioDev->open();
    m_audioDev->start();
    updatePlayState(MusicPlayerState::PlayingState);
}

void MusicPlayer::Impl::_pause()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_audioDev->stop();
    updatePlayState(MusicPlayerState::PausedState);
}

void MusicPlayer::Impl::_stop()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_audioDev->stop();
    m_audioDev->close();
    updatePlayState(MusicPlayerState::StoppedState);
}

void MusicPlayer::Impl::_setPosition(uint64_t pos)
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties && pos >= 0
        && pos <= m_curMusicProperties->signalProperties.durationMs) {
        AudioSpec &spec                                      = m_devSpec;
        m_curMusicProperties->signalProperties.curPositionMs = pos;
        m_curMusicProperties->signalProperties.curDataOffset
            = pos * spec.sampleRate * spec.bytesPerSample * spec.numChannel / 1000;
        m_listener->onMusicPlayerPositionChanged(pos);
    }
}

void MusicPlayer::Impl::_next()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_musicList->next();
}

void MusicPlayer::Impl::_previous()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_musicList->pervious();
}

void MusicPlayer::Impl::_setCurrentIndex(int index)
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_curMusicProperties == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_musicList->setCurrentIndex(index);
}

void MusicPlayer::Impl::getAudioData(void *data, int len)
{
    if (m_curMusicProperties) {
        SignalProperties &signalProperties = m_curMusicProperties->signalProperties;
        m_curMusicProperties->rawBuffer->getData(signalProperties.curDataOffset, len, (char *)data);
        AudioSpec &spec                 = m_devSpec;
        signalProperties.curDataOffset += len;
        signalProperties.curPositionMs
            += 1000LL * len / (spec.bytesPerSample * spec.numChannel) / spec.sampleRate;
        uint64_t curPositionMs = signalProperties.curPositionMs;
        m_workQueue.asyncRunTask(
            [this, curPositionMs]() { m_listener->onMusicPlayerPositionChanged(curPositionMs); });
        // LOG_DEBUG(LOG_TAG, "curDataOffset : %lld, len:%d", signalProperties.curDataOffset.load(),
        //           len);
        if (signalProperties.curDataOffset >= signalProperties.dataSize) {
            signalProperties.curDataOffset = 0;
            signalProperties.curPositionMs = 0;
            next();
        }
    }
}

void MusicPlayer::Impl::putMusicPlayListCurBuf(MusicPropertiesPtr property)
{
    LOG_INFO(LOG_TAG, "putMusicPlayListCurBuf : %d", property->index);
    m_curMusicProperties = property;
    m_listener->onMusicPlayerListCurrentIndexChanged(property->index);
    m_listener->onMusicPlayerDurationChanged(property->signalProperties.durationMs);
    m_listener->onMusicPlayerPositionChanged(property->signalProperties.curPositionMs);
}

void MusicPlayer::Impl::updateMusicList(std::vector<MusicPropertiesPtr> &list)
{
    LOG_INFO(LOG_TAG, "updateMusicList");
    m_musicListIndex.clear();
    for(auto &property : list) {
        MusicIndex index;
        index.index = property->index;
        index.name  = property->fileProperties.fileName;
        m_musicListIndex.push_back(index);
    }
    m_listener->onMusicPlayerMusicListChanged(m_musicListIndex);
}


void MusicPlayer::Impl::updatePlayState(MusicPlayerState state)
{
    m_state = state;
    m_listener->onMusicPlayerStateChanged(m_state);
}

MusicPlayer::MusicPlayer(MusicPlayerListener *listener, std::string &logDir)
    : m_impl(new Impl(listener, logDir))
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
