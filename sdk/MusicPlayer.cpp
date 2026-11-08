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

class MusicPlayer::Impl : public MusicPlayListCallback {
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
    // 关闭音频设备的唯一出口：_stop 与析构都走这里。调用方必须保证控制队列已静止
    // （析构先 m_workQueue.shutdown()），否则会与队列线程中的 _stop 并发关同一个设备。
    void _releaseAudio();
    void _setPosition(uint64_t pos);
    void _next();
    void _previous();
    void _setCurrentIndex(int index);

protected:
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property);
    virtual void updateMusicList(std::vector<MusicPropertiesPtr> &list);
    virtual void onMusicPlayListError(ErrorCode code, const std::string &detail, int index,
                                      const std::string &path);

private:
    void updatePlayState(MusicPlayerState state);
    void reportPosition(uint64_t positionMs, int trackIndex, bool resetFloor);
    std::string newTraceId();
    void bumpTraceId(const char *reason);
    bool shouldAutoSkipOnError() const;
    bool isShuttingDown() const;
    void fillSilence(void *data, int len) const;

    void startConsumerThread();
    void stopConsumerThread();
    void consumeAudioData();
    bool shouldFillSilence();
    void handleDecodeError(const MusicPropertiesPtr &cur);

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
    // 本次播放（一次 _play）里本次曲目的解码失败只上报一次，避免 10ms 消费循环刷屏
    std::atomic<bool> m_decodeErrorHandled;
    // 契约（规格 §3 / §5 / #88）：同曲会话内 onPositionChanged 单调不回退、换曲归零。
    // 六个回调都在同一条 WorkQueue 串行线程发出，这里只钳制二次 asyncRunTask
    // 回投 priority_queue 时同刻乱序带来的旧值；position() 查询口径不变。
    std::atomic<uint64_t> m_positionFloor;
    std::atomic<int> m_positionTrackIndex;
    WorkQueue m_workQueue;
    WorkQueue::TaskID m_consumerTaskID;
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
    , m_decodeErrorHandled(false)
    , m_positionFloor(0)
    , m_positionTrackIndex(-1)
    , m_workQueue()
    , m_consumerTaskID(WorkQueue::TaskID_Error)
{
    SdkLogConfig logConfig;
    logConfig.directory           = logDir;
    logConfig.filePrefix          = "soundbridge";
    logConfig.singleFileSizeBytes = 10 * 1024 * 1024;
    logConfig.maxFileCount        = 20;
    InitializeLogging(logConfig);
    LOG_INFO(LOG_TAG, "Log init success");

    m_audioDev = std::make_shared<AudioDevice>();
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
    stopConsumerThread();
    // 先静止控制队列再动设备。m_workQueue 是成员，自身析构里的 stop()+join 发生在
    // 本函数体之后；若不等它退出，函数体里的关设备会与队列线程中尚未执行完的 _stop
    // 并发进入 SDL_CloseAudioDevice，同一个 m_deviceID 被释放两次（issue #70）。
    // 这也是规格 4.4「析构是 join 唯一例外」的落点：函数体内完成 join，
    // 之后不会再有任何控制任务，_releaseAudio 成为唯一执行者。
    m_workQueue.shutdown();
    _releaseAudio();
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
                // 被拒文件不进播放列表，永远不会成为当前曲目，只能靠这份汇总让它们可见
                m_musicList->reportScanSummary();
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

    m_workQueue.asyncRunTask([this]() {
        // 用户主动播放 = 新的播放会话：上次因坏数据失败的曲目可以重新尝试
        m_musicList->clearFailedTracks();
        _play();
    });
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
    m_decodeErrorHandled.store(false);
    if (m_state == MusicPlayerState::StoppedState) {
        bumpTraceId("play");
        // 一次 _play 从停机起步 = 新的播放会话：位置单调基准必须跟着归零，
        // 否则 stop 后重播同曲（不经过 putMusicPlayListCurBuf）会被上一轮 floor 钳住。
        m_positionFloor.store(0);
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
    // play 负责备好本次播放任务：stop 之后 ring / decoder 已被丢弃，这里必须建新的；
    // 暂停后恢复则沿用还活着的那个任务（缓冲不白扔）。顺序遵循规格 4.1：
    // 任务先就绪，再开设备、再起消费线程。
    if (!m_musicList->startCurrentTask()) {
        onMusicPlayListError(ErrorCode::DecodeInitFailed, "failed to start playback task",
                             static_cast<int>(cur->index), cur->fileProperties.fullPath);
        return;
    }
    // 设备可能已经打开：暂停只 stop 不 close（恢复时沿用同一个 SDL 设备），
    // 这里若再 open 一次会以 "already open" 失败，把恢复误判成 AudioDeviceOpenFailed
    // 并触发自动跳过（表现为「继续播放变成跳下一首」）。
    if (!m_audioDev->isOpen() && m_audioDev->open() != 0) {
        onMusicPlayListError(ErrorCode::AudioDeviceOpenFailed, "audio device open failed",
                             static_cast<int>(cur->index), cur->fileProperties.fullPath);
        return;
    }
    if (m_audioDev->start() != 0) {
        onMusicPlayListError(ErrorCode::AudioDeviceStartFailed, "audio device start failed",
                             static_cast<int>(cur->index), cur->fileProperties.fullPath);
        return;
    }
    startConsumerThread();
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
    stopConsumerThread();
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
    stopConsumerThread();
    _releaseAudio();
    updatePlayState(MusicPlayerState::StoppedState);
}

void MusicPlayer::Impl::_releaseAudio()
{
    // 停止时整个播放任务被丢弃：解码线程在这里 join，旧 PCM 随 ring 一起消失，
    // 不 drain、不播完（规格 4.1 stop 行）。
    if (m_musicList != nullptr) {
        m_musicList->stopCurrentTask();
    }
    if (m_audioDev != nullptr) {
        m_audioDev->clearQueue();
        m_audioDev->stop();
        m_audioDev->close();
    }
}

void MusicPlayer::Impl::_setPosition(uint64_t pos)
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (cur == nullptr || task == nullptr || task->decoder == nullptr) {
        // 停止 / 切歌窗口里没有可 seek 的任务：显式丢弃这一次 seek（规格 4.1 seek 行），
        // 不能让它在下一个任务上「迟到生效」。
        LOG_INFO(LOG_TAG, "seek dropped: no playback task");
        return;
    }
    if (task->trackIndex != cur->index) {
        LOG_INFO(LOG_TAG, "seek dropped: task belongs to another track");
        return;
    }
    if (pos > cur->signalProperties.durationMs) {
        return;
    }

    // 位置只有一个来源：解码器的段基准 + ring 段内读计数（见 AudioStreamDecoder::positionMs）。
    // 消费端不再自己维护字节偏移，避免 seek 目标与播放进度两套口径互相打架。
    cur->signalProperties.curPositionMs = pos;
    task->decoder->seekToMs(pos);
    // seek 是合法回退：重置单调基准并立即上报一次（规格 §3 / §7）。
    reportPosition(pos, static_cast<int>(cur->index), true);
}

void MusicPlayer::Impl::_next()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    bumpTraceId("next");
    m_switching.store(true);
    m_audioDev->clearQueue();
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
    m_audioDev->clearQueue();
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

bool MusicPlayer::Impl::shouldFillSilence()
{
    if (isShuttingDown() || m_switching.load()) {
        return true;
    }
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (task == nullptr || task->ring == nullptr || task->decoder == nullptr) {
        // 没有播放任务 = stop 之后 / 切歌窗口：这一拍只能出静音。
        return true;
    }
    if (task->decoder->state() == StreamDecoderState::SEEKING) {
        return true;
    }
    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    // 任务与「当前曲目」对不上：切歌通知还没走完，读 ring 就会把上一首的 PCM 送出去。
    if (cur == nullptr || task->trackIndex != cur->index) {
        return true;
    }
    return false;
}

void MusicPlayer::Impl::consumeAudioData()
{
    if (shouldFillSilence()) {
        const size_t len = 4096;
        std::vector<char> silence(len, 0);
        m_audioDev->write(silence.data(), len);
        // 观测点（规格第 5 节 / 票面 #56）：静音填充次数记在当前曲目的解码器上。
        // 只记账、不改任何分支；解码器只统计静默窗口内的调用，
        // 所以稳态播放期间这个数恒为 0（判据的口径是「稳态窗口静音填充为零」）。
        MusicPropertiesPtr cur;
        {
            std::lock_guard<std::mutex> lock(m_curMusicMutex);
            cur = m_curMusicProperties;
        }
        PlaybackTaskPtr task = m_musicList->currentTask();
        if (task != nullptr && task->decoder != nullptr) {
            task->decoder->noteSilenceFill();
        }
        return;
    }

    MusicPropertiesPtr cur;
    {
        std::lock_guard<std::mutex> lock(m_curMusicMutex);
        cur = m_curMusicProperties;
    }
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (cur == nullptr || task == nullptr || task->ring == nullptr || task->decoder == nullptr) {
        return;
    }

    AudioRingBuffer *ring = task->ring.get();

    const size_t kQueueThreshold = 256 * 1024;
    if (m_audioDev->getQueuedBytes() > kQueueThreshold) {
        return;
    }

    const size_t kChunkSize = 4096;
    std::vector<char> buf(kChunkSize);
    size_t got = ring->read(buf.data(), kChunkSize);

    if (got > 0) {
        m_audioDev->write(buf.data(), got);

        // 单一位置来源：解码器按「段基准 + ring 段内已读字节」换算。
        // 消费端只负责告知时间，不再累计自己的字节偏移（seek / 切歌时两套计数会漂移）。
        SignalProperties &signalProperties = cur->signalProperties;
        const uint64_t curPositionMs       = task->decoder->positionMs();
        signalProperties.curPositionMs     = curPositionMs;

        if (!isShuttingDown()) {
            const int trackIndex = static_cast<int>(cur->index);
            m_workQueue.asyncRunTask([this, curPositionMs, trackIndex]() {
                // 旧曲的迟到任务不得写进新曲会话（换曲归零后基准已重置）。
                if (!isShuttingDown() && trackIndex == m_positionTrackIndex.load()) {
                    reportPosition(curPositionMs, trackIndex, false);
                }
            });
        }
    }

    StreamDecoderState st = task->decoder->state();
    if (st == StreamDecoderState::ERROR) {
        // 已解出的 PCM 先播完再上报：报出来的位置必须是用户实际听到的位置（基石 B4）
        if (ring->availableRead() == 0) {
            handleDecodeError(cur);
        }
        return;
    }

    if (st == StreamDecoderState::EOS && ring->availableRead() == 0) {
        if (!isShuttingDown() && !m_switching.load() && !m_switching.exchange(true)) {
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

void MusicPlayer::Impl::handleDecodeError(const MusicPropertiesPtr &cur)
{
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (cur == nullptr || task == nullptr || task->decoder == nullptr
        || task->trackIndex != cur->index) {
        return;
    }
    if (m_decodeErrorHandled.exchange(true)) {
        return;
    }

    const ErrorCode code       = task->decoder->errorCode();
    const std::string detail   = task->decoder->errorDetail();
    const ErrorCodeInfo &info  = GetErrorInfo(code);
    const uint64_t positionMs  = cur->signalProperties.curPositionMs.load();
    const bool canAutoSkip     = (info.action == ErrorAction::SkipTrack) && shouldAutoSkipOnError();
    LogPrintfWithTrace(SdkLogLevel::Error, LOG_TAG, m_traceId,
                       "decode error code=%d detail=%s index=%d positionMs=%llu path=%s",
                       static_cast<int>(code), detail.c_str(), static_cast<int>(cur->index),
                       static_cast<unsigned long long>(positionMs),
                       cur->fileProperties.fullPath.c_str());

    // 本次播放会话内不再自动重试这一曲（规格 8.2 / 实现清单 21）
    m_musicList->markTrackFailed(cur->index);
    if (m_listener != nullptr) {
        m_listener->onMusicPlayerError(code, detail, static_cast<int>(cur->index),
                                       cur->fileProperties.fullPath, m_traceId);
    }

    stopConsumerThread();
    m_audioDev->stop();
    m_audioDev->clearQueue();
    m_audioDev->close();

    if (!canAutoSkip) {
        // 停在坏处：位置留在 curPositionMs，状态置 Paused（规格 8.2）
        updatePlayState(MusicPlayerState::PausedState);
        return;
    }

    m_switching.store(true);
    m_workQueue.asyncRunTask([this]() {
        bool advanced = (m_musicList != nullptr) && m_musicList->skipToNextPlayable();
        m_switching.store(false);
        if (advanced) {
            _play();
        } else {
            updatePlayState(MusicPlayerState::PausedState);
        }
    });
}

void MusicPlayer::Impl::startConsumerThread()
{
    if (m_consumerTaskID != WorkQueue::TaskID_Error) {
        return;
    }
    m_consumerTaskID = m_workQueue.startTimerTask(true, 10, [this]() { this->consumeAudioData(); });
    LOG_INFO(LOG_TAG, "Consumer thread started, taskID=%zu", m_consumerTaskID);
}

void MusicPlayer::Impl::stopConsumerThread()
{
    if (m_consumerTaskID == WorkQueue::TaskID_Error) {
        return;
    }
    m_workQueue.stopTimerTask(m_consumerTaskID);
    m_consumerTaskID = WorkQueue::TaskID_Error;
    LOG_INFO(LOG_TAG, "Consumer thread stopped");
}

void MusicPlayer::Impl::putMusicPlayListCurBuf(MusicPropertiesPtr property)
{
    if (isShuttingDown() || property == nullptr) {
        return;
    }
    LogPrintfWithTrace(SdkLogLevel::Info, LOG_TAG, m_traceId, "putMusicPlayListCurBuf : %d(%s)",
                       property->index, property->fileProperties.fileName.data());
    m_skipFailures.store(0);
    m_decodeErrorHandled.store(false);
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
    // 新曲会话：重置单调基准，位置从该曲的 curPositionMs（通常 0）重新起算。
    reportPosition(property->signalProperties.curPositionMs,
                   static_cast<int>(property->index), true);
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
    if (index < 0) {
        // 批量扫描汇总：不绑定曲目、不改变播放状态，只是让被拒文件可见
        std::string summary = FormatError(code, detail);
        LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, m_traceId,
                           "scan summary code=%d message=%s", static_cast<int>(code),
                           summary.c_str());
        if (m_listener != nullptr) {
            m_listener->onMusicPlayerError(code, detail, index, path, m_traceId);
        }
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
    // 契约（doc/player-state-contract.md §3 / #87）：原因先于结果，onError 在状态变化之前。
    // autoSkip 跳轨成功时状态可能根本不变；失败时这里同步置 Stopped 就是那「一次状态事件」，
    // 不再补发（updatePlayState 去重保证状态序列无相邻重复）。
    if (m_listener != nullptr) {
        m_listener->onMusicPlayerError(code, detail, index, path, m_traceId);
    }
    updatePlayState(MusicPlayerState::StoppedState);
    // 规格 §6.3 / #90：与解码错误路径的 handleDecodeError 统一，自动跳轨以
    // ErrorAction::SkipTrack 为准。设备错误是 StopPlayback，不该靠跳轨绕过。
    const bool canAutoSkip = shouldAutoSkipOnError()
        && GetErrorInfo(code).action == ErrorAction::SkipTrack;
    if (canAutoSkip) {
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

void MusicPlayer::Impl::reportPosition(uint64_t positionMs, int trackIndex, bool resetFloor)
{
    if (isShuttingDown() || m_listener == nullptr) {
        return;
    }
    // 回调单线程串行（#76 事实基线），普通 load/store 即可；这里只做通知侧的钳制，
    // 不回写 signalProperties.curPositionMs，也不影响 position() 的实时查询。
    if (resetFloor || trackIndex != m_positionTrackIndex.load()) {
        m_positionTrackIndex.store(trackIndex);
        m_positionFloor.store(positionMs);
        m_listener->onMusicPlayerPositionChanged(positionMs);
        return;
    }
    if (positionMs < m_positionFloor.load()) {
        return;
    }
    m_positionFloor.store(positionMs);
    m_listener->onMusicPlayerPositionChanged(positionMs);
}

void MusicPlayer::Impl::updatePlayState(MusicPlayerState state)
{
    if (isShuttingDown()) {
        return;
    }
    // 契约（规格 §3 / §4）：重复控制命令不重复发 onStateChanged，状态序列无相邻重复。
    // m_state 仍是唯一权威；相同值只跳过通知，不改变查询值。
    if (m_state.load() == state) {
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
