#include "MusicPlayer.h"
#include "AudioBuffer.h"
#include "AudioDevice.h"
#include "FileSearch.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "WorkQueue.hpp"
#include <chrono>
#include <cstring>
#include <list>
#include <mutex>
#include <sstream>

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
    void setAutoSkipOnError(bool enabled);
    bool autoSkipOnError() const;

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
    virtual void onMusicPlayListError(ErrorCode code, const std::string &detail, int index,
                                      const std::string &path);

private:
    void updatePlayState(MusicPlayerState state);
    std::string newTraceId();
    void bumpTraceId(const char *reason);
    bool shouldAutoSkipOnError() const;

private:
    WorkQueue m_workQueue;
    std::shared_ptr<AudioDevice> m_audioDev;
    AudioSpec m_devSpec;
    std::shared_ptr<MusicPlayList> m_musicList;
    MusicPlayerListener *m_listener;
    MusicPlayerState m_state;
    MusicPropertiesPtr m_curMusicProperties;
    std::mutex m_curMusicMutex;
    std::list<MusicIndex> m_musicListIndex;
    std::atomic<int> m_pendingAdd;
    std::string m_traceId;
    std::atomic<uint64_t> m_traceSeq;
    std::atomic<bool> m_autoSkipOnError;
    std::atomic<int> m_skipFailures;
    std::atomic<bool> m_switching;
};

MusicPlayer::Impl::Impl(MusicPlayerListener *lister, std::string &logDir)
    : m_workQueue()
    , m_audioDev(nullptr)
    , m_musicList(nullptr)
    , m_listener(lister)
    , m_state(MusicPlayerState::StoppedState)
    , m_pendingAdd(0)
    , m_traceSeq(0)
    , m_autoSkipOnError(true)
    , m_skipFailures(0)
    , m_switching(false)
{
    SdkLogConfig logConfig;
    logConfig.directory           = logDir;
    logConfig.filePrefix          = "soundbridge";
    logConfig.singleFileSizeBytes = 10 * 1024 * 1024;
    logConfig.maxFileCount        = 20;
    InitializeLogging(logConfig);
    LOG_INFO(LOG_TAG, "Log init success");

    m_audioDev = std::make_shared<AudioDevice>(this);
    m_audioDev->getDeviceSpec(m_devSpec);
    m_musicList = std::make_shared<MusicPlayList>(this, &m_workQueue, m_devSpec);
    m_traceId   = newTraceId();
    m_musicList->setTraceId(m_traceId);
    LOG_INFO(LOG_TAG, "Impl construct");
}

void MusicPlayer::Impl::addMusicDir(const std::string &dir)
{
    LOG_INFO(LOG_TAG, "addMusicDir : %s", dir.data());
    std::vector<std::string> musicList = recursiveFileSearch(dir);
    m_pendingAdd.store(static_cast<int>(musicList.size()));
    if (musicList.empty()) {
        m_musicList->updateList();
        return;
    }
    for (auto &path : musicList) {
        m_musicList->addMusicWithNotify(path, [this]() {
            if (m_pendingAdd.fetch_sub(1) == 1) {
                m_musicList->updateList();
            }
        });
    }
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

void MusicPlayer::Impl::setAutoSkipOnError(bool enabled)
{
    m_autoSkipOnError.store(enabled);
}

bool MusicPlayer::Impl::autoSkipOnError() const
{
    return m_autoSkipOnError.load();
}

void MusicPlayer::Impl::_addMusic(const std::string &musicPath) { }

void MusicPlayer::Impl::_play()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    if (m_state == MusicPlayerState::StoppedState) {
        bumpTraceId("play");
    }
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        if (m_listener != nullptr) {
            m_listener->onMusicPlayerError(ErrorCode::PlayerNoCurrent, "no current track", -1, "",
                                           m_traceId);
        }
        return;
    }
    if (m_audioDev->open() != 0) {
        onMusicPlayListError(ErrorCode::AudioDeviceOpenFailed, "audio device open failed",
                             static_cast<int>(cur->index), cur->fileProperties.fullPath);
        return;
    }
    if (m_audioDev->start() != 0) {
        onMusicPlayListError(ErrorCode::AudioDeviceStartFailed, "audio device start failed",
                             static_cast<int>(cur->index), cur->fileProperties.fullPath);
        return;
    }
    updatePlayState(MusicPlayerState::PlayingState);
}

void MusicPlayer::Impl::_pause()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        return;
    }
    m_audioDev->stop();
    updatePlayState(MusicPlayerState::PausedState);
}

void MusicPlayer::Impl::_stop()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
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
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur && pos >= 0 && pos <= cur->signalProperties.durationMs) {
        AudioSpec &spec                     = m_devSpec;
        cur->signalProperties.curPositionMs = pos;
        cur->signalProperties.curDataOffset
            = pos * spec.sampleRate * spec.bytesPerSample * spec.numChannel / 1000;
        m_listener->onMusicPlayerPositionChanged(pos);
    }
}

void MusicPlayer::Impl::_next()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    bumpTraceId("next");
    m_switching.store(true);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        m_switching.store(false);
        return;
    }
    m_musicList->next();
}

void MusicPlayer::Impl::_previous()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    bumpTraceId("previous");
    m_switching.store(true);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        m_switching.store(false);
        return;
    }
    m_musicList->pervious();
}

void MusicPlayer::Impl::_setCurrentIndex(int index)
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    bumpTraceId("setIndex");
    m_switching.store(true);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr) {
        LOG_ERROR(LOG_TAG, "m_curMusicProperties is nullptr");
        m_switching.store(false);
        return;
    }
    m_musicList->setCurrentIndex(index);
}

void MusicPlayer::Impl::getAudioData(void *data, int len)
{
    if (m_switching.load()) {
        if (data && len > 0) {
            std::memset(data, 0, len);
        }
        return;
    }
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur && cur->rawBuffer) {
        SignalProperties &signalProperties = cur->signalProperties;
        cur->rawBuffer->getData(signalProperties.curDataOffset, len, (char *)data);
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
    } else {
        if (data && len > 0) {
            std::memset(data, 0, len);
        }
    }
}

void MusicPlayer::Impl::putMusicPlayListCurBuf(MusicPropertiesPtr property)
{
    LogPrintfWithTrace(SdkLogLevel::Info, LOG_TAG, m_traceId, "putMusicPlayListCurBuf : %d(%s)",
                       property->index, property->fileProperties.fileName.data());
    m_skipFailures.store(0);
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        m_curMusicProperties = property;
    }
    m_switching.store(false);
    m_listener->onMusicPlayerListCurrentIndexChanged(property->index);
    m_listener->onMusicPlayerDurationChanged(property->signalProperties.durationMs);
    m_listener->onMusicPlayerPositionChanged(property->signalProperties.curPositionMs);
}

void MusicPlayer::Impl::updateMusicList(std::vector<MusicPropertiesPtr> &list)
{
    LOG_INFO(LOG_TAG, "updateMusicList list size:%d", list.size());
    m_musicListIndex.clear();
    for (auto &property : list) {
        MusicIndex index;
        index.index = property->index;
        index.name  = property->fileProperties.fileName;
        m_musicListIndex.push_back(index);
    }
    m_listener->onMusicPlayerMusicListChanged(m_musicListIndex);
}

void MusicPlayer::Impl::onMusicPlayListError(ErrorCode code, const std::string &detail, int index,
                                             const std::string &path)
{
    m_switching.store(false);
    std::string message = FormatError(code, detail);
    LogPrintfWithTrace(SdkLogLevel::Error, LOG_TAG, m_traceId,
                       "playlist error code=%d message=%s index=%d path=%s", static_cast<int>(code),
                       message.c_str(), index, path.c_str());
    if (m_audioDev != nullptr) {
        m_audioDev->stop();
        m_audioDev->close();
    }
    updatePlayState(MusicPlayerState::StoppedState);
    if (m_listener != nullptr) {
        m_listener->onMusicPlayerError(code, detail, index, path, m_traceId);
    }
    if (shouldAutoSkipOnError()) {
        m_skipFailures.fetch_add(1);
        m_workQueue.asyncRunTask([this]() {
            if (m_musicList->skipToNextPlayable()) {
                _play();
            } else {
                LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, m_traceId,
                                   "auto-skip failed: no playable track");
            }
        });
    }
}

void MusicPlayer::Impl::updatePlayState(MusicPlayerState state)
{
    m_state = state;
    m_listener->onMusicPlayerStateChanged(m_state);
}

bool MusicPlayer::Impl::shouldAutoSkipOnError() const
{
    if (!m_autoSkipOnError.load()) {
        return false;
    }
    const int count = m_musicList ? m_musicList->getMusicCount() : 0;
    if (count <= 1) {
        return false;
    }
    const int failures = m_skipFailures.load();
    return failures < count;
}

std::string MusicPlayer::Impl::newTraceId()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    uint64_t seq = m_traceSeq.fetch_add(1) + 1;
    std::ostringstream oss;
    oss << ms << "-" << seq;
    return oss.str();
}

void MusicPlayer::Impl::bumpTraceId(const char *reason)
{
    m_traceId = newTraceId();
    m_musicList->setTraceId(m_traceId);
    LogPrintfWithTrace(SdkLogLevel::Info, LOG_TAG, m_traceId, "trace start reason=%s",
                       reason ? reason : "unknown");
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

void MusicPlayer::setAutoSkipOnError(bool enabled)
{
    m_impl->setAutoSkipOnError(enabled);
}

bool MusicPlayer::autoSkipOnError() const
{
    return m_impl->autoSkipOnError();
}
}
