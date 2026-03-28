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
    ~Impl();
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
    void setPlaybackMode(MusicPlaybackMode mode);
    MusicPlaybackMode playbackMode() const;
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
    bool isShuttingDown() const;
    void fillSilence(void *data, int len) const;

private:
    std::shared_ptr<AudioDevice> m_audioDev;
    AudioSpec m_devSpec;
    std::shared_ptr<MusicPlayList> m_musicList;
    MusicPlayerListener *m_listener;
    std::atomic<MusicPlayerState> m_state;
    MusicPropertiesPtr m_curMusicProperties;
    std::mutex m_curMusicMutex;
    std::list<MusicIndex> m_musicListIndex;
    std::atomic<int> m_pendingAdd;
    std::string m_traceId;
    std::atomic<uint64_t> m_traceSeq;
    std::atomic<bool> m_autoSkipOnError;
    std::atomic<int> m_skipFailures;
    std::atomic<bool> m_switching;
    std::atomic<bool> m_shuttingDown;
    WorkQueue m_workQueue;
};

MusicPlayer::Impl::Impl(MusicPlayerListener *lister, std::string &logDir)
    : m_audioDev(nullptr)
    , m_musicList(nullptr)
    , m_listener(lister)
    , m_state(MusicPlayerState::StoppedState)
    , m_pendingAdd(0)
    , m_traceSeq(0)
    , m_autoSkipOnError(true)
    , m_skipFailures(0)
    , m_switching(false)
    , m_shuttingDown(false)
    , m_workQueue()
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
    m_musicList->setPlaybackMode(MusicPlaybackMode::Sequential);
    m_traceId = newTraceId();
    m_musicList->setTraceId(m_traceId);
    LOG_INFO(LOG_TAG, "Impl construct");
}

MusicPlayer::Impl::~Impl()
{
    m_shuttingDown.store(true);
    m_switching.store(true);
    if (m_audioDev != nullptr) {
        m_audioDev->stop();
        m_audioDev->close();
    }
}

void MusicPlayer::Impl::addMusicDir(const std::string &dir)
{
    LOG_INFO(LOG_TAG, "addMusicDir : %s", dir.data());
    if (isShuttingDown()) {
        return;
    }
    std::vector<std::string> musicList = recursiveFileSearch(dir);
    m_pendingAdd.store(static_cast<int>(musicList.size()));
    if (musicList.empty()) {
        m_musicList->updateList();
        return;
    }
    for (auto &path : musicList) {
        m_musicList->addMusicWithNotify(path, [this]() {
            if (isShuttingDown()) {
                return;
            }
            if (m_pendingAdd.fetch_sub(1) == 1) {
                m_musicList->updateList();
            }
        });
    }
}

void MusicPlayer::Impl::play()
{
    LOG_INFO(LOG_TAG, "play");
    if (isShuttingDown()) {
        return;
    }

    m_workQueue.asyncRunTask([this]() { _play(); });
}

void MusicPlayer::Impl::pause()
{
    LOG_INFO(LOG_TAG, "pause");
    if (isShuttingDown()) {
        return;
    }

    m_workQueue.asyncRunTask([this]() { _pause(); });
}

void MusicPlayer::Impl::stop()
{
    LOG_INFO(LOG_TAG, "stop");
    if (isShuttingDown()) {
        return;
    }

    m_workQueue.asyncRunTask([this]() { _stop(); });
}

MusicPlayerState MusicPlayer::Impl::state()
{
    return m_state.load();
}

void MusicPlayer::Impl::setPosition(uint64_t pos)
{
    LOG_INFO(LOG_TAG, "setPosition : %llu", pos);
    if (isShuttingDown()) {
        return;
    }
    m_workQueue.asyncRunTask([this, pos]() { _setPosition(pos); });
}

void MusicPlayer::Impl::next()
{
    if (isShuttingDown()) {
        return;
    }
    m_workQueue.asyncRunTask([this]() { _next(); });
}

void MusicPlayer::Impl::previous()
{
    if (isShuttingDown()) {
        return;
    }
    m_workQueue.asyncRunTask([this]() { _previous(); });
}

void MusicPlayer::Impl::setCurrentIndex(int index)
{
    if (isShuttingDown()) {
        return;
    }
    m_workQueue.asyncRunTask([this, index]() { _setCurrentIndex(index); });
}

int MusicPlayer::Impl::getMusicCount()
{
    return m_musicList->getMusicCount();
}

void MusicPlayer::Impl::setPlaybackMode(MusicPlaybackMode mode)
{
    if (m_musicList != nullptr) {
        m_musicList->setPlaybackMode(mode);
    }
}

MusicPlaybackMode MusicPlayer::Impl::playbackMode() const
{
    if (m_musicList != nullptr) {
        return m_musicList->playbackMode();
    }
    return MusicPlaybackMode::Sequential;
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
    if (cur == nullptr || cur->streamDecoder == nullptr || pos > cur->signalProperties.durationMs) {
        return;
    }

    AudioSpec &spec = m_devSpec;
    size_t byteOffset
        = static_cast<size_t>(pos * spec.sampleRate * spec.numChannel * spec.bytesPerSample / 1000);
    cur->signalProperties.curDataOffset.store(static_cast<off64_t>(byteOffset));
    cur->signalProperties.curPositionMs = pos;
    cur->streamDecoder->seekToOffset(byteOffset);
    m_listener->onMusicPlayerPositionChanged(pos);
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
    if (!m_musicList->nextSync()) {
        m_switching.store(false);
    }
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
    if (!m_musicList->previousSync()) {
        m_switching.store(false);
    }
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
    if (!m_musicList->setCurrentIndexSync(index)) {
        m_switching.store(false);
    }
}

void MusicPlayer::Impl::getAudioData(void *data, int len)
{
    if (isShuttingDown() || m_switching.load()) {
        fillSilence(data, len);
        return;
    }

    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    if (cur == nullptr || cur->ringBuffer == nullptr || cur->streamDecoder == nullptr) {
        fillSilence(data, len);
        return;
    }

    AudioRingBuffer *ring = cur->ringBuffer.get();
    size_t got            = ring->read(static_cast<char *>(data), static_cast<size_t>(len));
    if (got < static_cast<size_t>(len)) {
        std::memset(static_cast<char *>(data) + got, 0, static_cast<size_t>(len) - got);
    }

    SignalProperties &signalProperties = cur->signalProperties;
    AudioSpec &spec                    = m_devSpec;
    signalProperties.curDataOffset.fetch_add(static_cast<off64_t>(got), std::memory_order_relaxed);
    uint64_t consumed   = static_cast<uint64_t>(signalProperties.curDataOffset.load());
    uint64_t bytesPerMs = static_cast<uint64_t>(spec.sampleRate)
        * static_cast<uint64_t>(spec.numChannel) * static_cast<uint64_t>(spec.bytesPerSample)
        / 1000;
    signalProperties.curPositionMs = (bytesPerMs > 0) ? (consumed / bytesPerMs) : 0;

    uint64_t curPositionMs = signalProperties.curPositionMs;
    if (!isShuttingDown()) {
        m_workQueue.asyncRunTask([this, curPositionMs]() {
            if (!isShuttingDown()) {
                m_listener->onMusicPlayerPositionChanged(curPositionMs);
            }
        });
    }

    StreamDecoderState st = cur->streamDecoder->state();
    if (st == StreamDecoderState::EOS && ring->availableRead() == 0) {
        if (!isShuttingDown() && !m_switching.exchange(true)) {
            m_workQueue.asyncRunTask([this]() {
                if (!isShuttingDown()) {
                    if (!m_musicList->advanceToNextTrack()) {
                        _stop();
                    }
                }
                m_switching.store(false);
            });
        }
    }
}

void MusicPlayer::Impl::putMusicPlayListCurBuf(MusicPropertiesPtr property)
{
    if (isShuttingDown() || property == nullptr) {
        return;
    }
    LogPrintfWithTrace(SdkLogLevel::Info, LOG_TAG, m_traceId, "putMusicPlayListCurBuf : %d(%s)",
                       property->index, property->fileProperties.fileName.data());
    m_skipFailures.store(0);
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        m_curMusicProperties = property;
    }
    m_switching.store(false);
    if (isShuttingDown()) {
        return;
    }
    m_listener->onMusicPlayerListCurrentIndexChanged(property->index);
    m_listener->onMusicPlayerDurationChanged(property->signalProperties.durationMs);
    m_listener->onMusicPlayerPositionChanged(property->signalProperties.curPositionMs);
}

void MusicPlayer::Impl::updateMusicList(std::vector<MusicPropertiesPtr> &list)
{
    if (isShuttingDown()) {
        return;
    }
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
    if (isShuttingDown()) {
        return;
    }
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
            if (isShuttingDown()) {
                return;
            }
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
    if (isShuttingDown()) {
        return;
    }
    m_state.store(state);
    m_listener->onMusicPlayerStateChanged(state);
}

bool MusicPlayer::Impl::shouldAutoSkipOnError() const
{
    if (isShuttingDown()) {
        return false;
    }
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

bool MusicPlayer::Impl::isShuttingDown() const
{
    return m_shuttingDown.load();
}

void MusicPlayer::Impl::fillSilence(void *data, int len) const
{
    if (data != nullptr && len > 0) {
        std::memset(data, 0, len);
    }
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

void MusicPlayer::setPlaybackMode(MusicPlaybackMode mode)
{
    m_impl->setPlaybackMode(mode);
}

MusicPlaybackMode MusicPlayer::playbackMode() const
{
    return m_impl->playbackMode();
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
