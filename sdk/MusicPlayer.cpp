#include "MusicPlayer.h"
#include "AudioBuffer.h"
#include "AudioDevice.h"
#include "FileSearch.h"
#include "IAudioDevice.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "PcmGain.hpp"
#include "PrefillGate.h"
#include "WorkQueue.hpp"
#include <chrono>
#include <cstring>
#include <list>
#include <mutex>
#include <sstream>
#include <utility>

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
    // 音量（软件增益，ADR 0013）：0–100，100 = 原样输出；静音与取值正交。
    void setVolume(int percent);
    int volume() const;
    void setMuted(bool muted);
    bool muted() const;
    size_t queuedBytesForTest() const;
    StreamMetrics metricsForTest() const;
    bool retiredTaskExpiredForTest() const;
    bool setAudioDeviceForTest(std::shared_ptr<IAudioDevice> device);

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
    void reportPosition(uint64_t positionMs, int trackIndex, bool resetFloor,
                        uint64_t epoch);
    void bumpPositionSession();
    std::string newTraceId();
    void bumpTraceId(const char *reason);
    bool shouldAutoSkipOnError() const;
    bool isShuttingDown() const;
    void fillSilence(void *data, int len) const;

    void startConsumerThread();
    void stopConsumerThread();
    void consumeAudioData();
    bool shouldFillSilence();
    // 当前生效增益：静音优先于取值。只读两个原子量，可在任意线程调用。
    float effectiveGain() const;
    // 首播门槛（规格 doc/buffering-switching-spec.md §2）：设备 start 前等 ring
    // 攒够 prefillMs；超时记仪表后照常起播。sourceSpec 是设备 spec 不可用时的回退来源。
    size_t prefillTargetBytes(const AudioSpec &sourceSpec) const;
    sdk_utils::PrefillResult waitForPrefill(const PlaybackTaskPtr &task,
                                            const AudioSpec &sourceSpec);
    // 用户发起的段切换：begin 记 trace + 落 switching 门闩；end 只在切换真的发生时净空
    // 设备队列（规格 §3.2）。切换失败（无下一首 / 索引越界）保留正在播的 PCM。
    void beginSegmentSwitch(const char *reason);
    void endSegmentSwitch(bool switched);
    void handleDecodeError(const MusicPropertiesPtr &cur);

private:
    std::shared_ptr<IAudioDevice> m_audioDev;
    // 仅供测试（ADR 0011）：被 setAudioDeviceForTest 替换掉的真实设备。必须保留它——
    // AudioDevice 的析构会无条件 SDL_Quit()，一旦被换下的设备当场销毁，替身的内层设备
    // 再 open 就会拿到「Audio subsystem is not initialized」。
    std::shared_ptr<IAudioDevice> m_replacedAudioDev;
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
    // 音量（ADR 0013）：控制线程写、消费者线程读，只用 relaxed——单值、无跨变量次序要求。
    std::atomic<int> m_volumePercent;
    std::atomic<bool> m_muted;
    // 消费线程独占：上一块实际使用的增益，用作下一块线性渐变的起点（消 zipper）。
    float m_appliedGain;
    // 格式不支持的告警只发一次，否则每 10 ms 一条会刷屏（ADR 0013）。
    bool m_gainFormatWarned;
    std::atomic<int> m_skipFailures;
    std::atomic<bool> m_switching;
    // 首播门槛「同一任务内只等一次」的身份标记（规格 §2.3）：暂停后恢复、重复 play()
    // 复用同一个 PlaybackTask，不再等第二次门闩（设备已在跑时再等只会超时）。
    // weak_ptr 比较任务身份：任务销毁后自动失效，不会把新任务误判成旧任务。
    // 读写只在 WorkQueue 线程（_play / consumeAudioData 同线程），不加锁。
    std::weak_ptr<PlaybackTask> m_prefillWaitedTask;
    std::atomic<bool> m_shuttingDown;
    // 本次播放（一次 _play）里本次曲目的解码失败只上报一次，避免 10ms 消费循环刷屏
    std::atomic<bool> m_decodeErrorHandled;
    // 契约（规格 §3 / §5 / #88）：同曲会话内 onPositionChanged 单调不回退、换曲归零。
    // 六个回调都在同一条 WorkQueue 串行线程发出，这里只钳制二次 asyncRunTask
    // 回投 priority_queue 时同刻乱序带来的旧值；position() 查询口径不变。
    std::atomic<uint64_t> m_positionFloor;
    std::atomic<int> m_positionTrackIndex;
    // 会话代际：seek / 换曲 / 从停机重播都会递增，让回投队列里同曲的旧任务整体失效，
    // 否则 reset 之前已投递、携带较大值的 position 任务会把新会话的基准重新抬高。
    std::atomic<uint64_t> m_positionEpoch;
    // 状态通知去重以「最近真正发出去的事件」为键，而不是 m_state 的初值：
    // 初值 Stopped 从未对外发过，首播失败 / skip 失败时仍必须通知一次。
    bool m_stateNotified;
    MusicPlayerState m_lastReportedState;
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
    , m_volumePercent(100)
    , m_muted(false)
    , m_appliedGain(1.0f)
    , m_gainFormatWarned(false)
    , m_skipFailures(0)
    , m_switching(false)
    , m_shuttingDown(false)
    , m_decodeErrorHandled(false)
    , m_positionFloor(0)
    , m_positionTrackIndex(-1)
    , m_positionEpoch(0)
    , m_stateNotified(false)
    , m_lastReportedState(MusicPlayerState::StoppedState)
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

void MusicPlayer::Impl::setVolume(int percent)
{
    // 越界钳制到端点而不是报错：音量是连续调节量，调用方（滑杆）不会刻意越界，
    // 真越界多半是持久化里的脏值，钳制比让整条命令失败更合适（规格 §8.5）。
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    m_volumePercent.store(percent, std::memory_order_relaxed);
}

int MusicPlayer::Impl::volume() const
{
    return m_volumePercent.load(std::memory_order_relaxed);
}

void MusicPlayer::Impl::setMuted(bool muted)
{
    m_muted.store(muted, std::memory_order_relaxed);
}

bool MusicPlayer::Impl::muted() const
{
    return m_muted.load(std::memory_order_relaxed);
}

float MusicPlayer::Impl::effectiveGain() const
{
    if (m_muted.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    return static_cast<float>(m_volumePercent.load(std::memory_order_relaxed)) / 100.0f;
}

size_t MusicPlayer::Impl::queuedBytesForTest() const
{
    // getQueuedBytes() 在设备未 open 时本身返回 0，这里不特判。
    return m_audioDev != nullptr ? m_audioDev->getQueuedBytes() : 0;
}

StreamMetrics MusicPlayer::Impl::metricsForTest() const
{
    if (m_musicList == nullptr) {
        return StreamMetrics {};
    }
    PlaybackTaskPtr task = m_musicList->currentTask();
    if (task == nullptr || task->decoder == nullptr) {
        return StreamMetrics {};
    }
    return task->decoder->metrics();
}

bool MusicPlayer::Impl::retiredTaskExpiredForTest() const
{
    if (m_musicList == nullptr) {
        return true;
    }
    return m_musicList->retiredTaskForTest().expired();
}

bool MusicPlayer::Impl::setAudioDeviceForTest(std::shared_ptr<IAudioDevice> device)
{
    if (device == nullptr) {
        LOG_ERROR(LOG_TAG, "reject null audio device");
        return false;
    }
    if (m_audioDev != nullptr && m_audioDev->isOpen()) {
        LOG_ERROR(LOG_TAG, "audio device is already open, reject test device");
        return false;
    }
    m_replacedAudioDev = m_audioDev;
    m_audioDev         = std::move(device);
    return true;
}

void MusicPlayer::Impl::_addMusic(const std::string &musicPath) { }

void MusicPlayer::Impl::_play()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    m_decodeErrorHandled.store(false);
    if (m_state == MusicPlayerState::StoppedState) {
        bumpTraceId("play");
        // 一次 _play 从停机起步 = 新的播放会话：递增代际并归零基准，
        // 让在途旧任务失效，且重播同曲（不经过 putMusicPlayListCurBuf）不被上一轮 floor 钳住。
        bumpPositionSession();
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
    // 首播门槛（规格 §2.2 第 3 步）：open 之后、start 之前等 ring 攒够一段 PCM。
    // 超时不报错：照常起播，SDL 用静音补齐；网络源首帧慢时这是常态。
    const PlaybackTaskPtr gateTask         = m_musicList->currentTask();
    const sdk_utils::PrefillResult prefill = waitForPrefill(gateTask, cur->signalProperties.spec);
    if (prefill == sdk_utils::PrefillResult::Aborted && gateTask != nullptr
        && gateTask->decoder != nullptr && gateTask->decoder->state() == StreamDecoderState::ERROR) {
        // 等待期解码线程已进 ERROR：设备不必起，交给既有失败分支（§2.3）。
        handleDecodeError(cur);
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
    // 净空设备队列：里面最多还有 256 KB 旧位置的 PCM（≈1.49 s @44.1k/2ch/S16），
    // 不清就会先播完旧位置才听到新位置（规格 §3.1）。seek 不设 m_switching：
    // SEEKING 已有独立静默窗口，叠加会改变 shouldFillSilence() 的既有口径。
    if (m_audioDev != nullptr) {
        m_audioDev->clearQueue();
    }
    task->decoder->seekToMs(pos);
    // seek 是合法回退：重置单调基准并立即上报一次（规格 §3 / §7）。
    bumpPositionSession();
    reportPosition(pos, static_cast<int>(cur->index), true, m_positionEpoch.load());
}

void MusicPlayer::Impl::_next()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    beginSegmentSwitch("next");
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
    endSegmentSwitch(m_musicList->nextSync());
}

void MusicPlayer::Impl::_previous()
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    beginSegmentSwitch("previous");
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
    endSegmentSwitch(m_musicList->previousSync());
}

void MusicPlayer::Impl::_setCurrentIndex(int index)
{
    LOG_INFO(LOG_TAG, "%s", __func__);
    beginSegmentSwitch("setIndex");
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
    endSegmentSwitch(m_musicList->setCurrentIndexSync(index));
}

void MusicPlayer::Impl::beginSegmentSwitch(const char *reason)
{
    bumpTraceId(reason);
    m_switching.store(true);
}

void MusicPlayer::Impl::endSegmentSwitch(bool switched)
{
    if (!switched) {
        // 切换没发生（无下一首 / 索引越界）：设备队列里正在播的 PCM 必须留着，
        // 否则「点了下一首但列表到头」会变成一次人为欠流（规格 §3.2）。
        m_switching.store(false);
        return;
    }
    // 净空时机：切换是同步完成的，消费拍与本函数同处一条 WorkQueue 线程，
    // 两者之间插不进任何写入，所以「切换后净空」与「切换前净空」对设备队列等价；
    // 而失败路径因此不再白白清空。队列里最多是旧位置的 256 KB（≈1.49 s，§3.1）。
    if (m_audioDev != nullptr) {
        m_audioDev->clearQueue();
    }
}

size_t MusicPlayer::Impl::prefillTargetBytes(const AudioSpec &sourceSpec) const
{
    // 规格 §2.1 的算式：max(4096, rate × channels × bytesPerSample ÷ 1000 × 150)。
    // 必须先乘后除：44100/2/S16 得 26 460 B，与规格落点逐字节一致；先除后乘会把
    // 44100/1000 截成 44，少算 60 B。
    const uint64_t kPrefillMs = 150;
    const size_t kFloor       = 4096;
    auto bytesFor             = [kPrefillMs](const AudioSpec &spec) -> size_t {
        int bytesPerSample = spec.bytesPerSample;
        if (bytesPerSample <= 0) {
            bytesPerSample = getBytePreSampleByAudioFormat(spec.format);
        }
        if (spec.sampleRate <= 0 || spec.numChannel <= 0 || bytesPerSample <= 0) {
            return 0;
        }
        const uint64_t bytes = static_cast<uint64_t>(spec.sampleRate)
            * static_cast<uint64_t>(spec.numChannel) * static_cast<uint64_t>(bytesPerSample)
            * kPrefillMs / 1000;
        return static_cast<size_t>(bytes);
    };
    // 设备 spec 先算；设备没给出可用 PCM 格式时退回源 spec 同式换算；
    // 两者都不可用才用下限（§2.1 的三级回退）。
    size_t target = bytesFor(m_devSpec);
    if (target == 0) {
        target = bytesFor(sourceSpec);
    }
    return target > kFloor ? target : kFloor;
}

sdk_utils::PrefillResult MusicPlayer::Impl::waitForPrefill(const PlaybackTaskPtr &task,
                                                           const AudioSpec &sourceSpec)
{
    if (task == nullptr || task->ring == nullptr || task->decoder == nullptr) {
        return sdk_utils::PrefillResult::Aborted;
    }
    // 同一任务内不重复等待（§2.3）：暂停→恢复、重复 play() 都复用同一个任务，
    // ring 里的 PCM 还在，再等一次只会白等到 300 ms 超时并误计 prefillTimeouts。
    if (m_prefillWaitedTask.lock() == task) {
        return sdk_utils::PrefillResult::Reached;
    }
    const size_t target = prefillTargetBytes(sourceSpec);
    const sdk_utils::PrefillResult result = sdk_utils::waitForPrefill(
        *task->ring, target, 300, [&task]() {
            const StreamDecoderState st = task->decoder->state();
            return st == StreamDecoderState::ERROR || st == StreamDecoderState::IDLE;
        });
    if (result == sdk_utils::PrefillResult::TimedOut) {
        // 每个任务最多记一次：上面的身份标记保证同一任务不会走进这里第二次。
        task->decoder->notePrefillTimeout();
    }
    m_prefillWaitedTask = task;
    return result;
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
    // 退役槽非阻塞回收（规格 §5.1 / §5.3）：旧解码线程已经退出时立刻 join 并释放，
    // 让切换窗口在旧线程退出后即结束（窗口外回落到单任务峰值），而不是拖到下一次
    // 控制事件。线程还在跑（例如卡在网络读）时这里什么都不做——消费拍上绝不等。
    if (m_musicList != nullptr) {
        m_musicList->reapRetiredTaskIfFinished();
    }

    if (shouldFillSilence()) {
        const size_t len = 4096;
        std::vector<char> silence(len, 0);
        const int silenceWriteRet = m_audioDev->write(silence.data(), len);
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
            if (silenceWriteRet < 0) {
                // 规格 §4.1 对 deviceWriteFailures 的定义是无条件「write() 返回 < 0 的次数」，
                // 没有把静音分支排除在外：静音写失败同样说明设备侧出了问题，照记。
                task->decoder->noteDeviceWriteFailure();
            }
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
        // 软件增益（ADR 0013）：只在这一段做，覆盖全部设备写入点；默认 100% + 未静音时
        // 整段跳过，既有判据看到的字节与顺序完全不变。
        const float targetGain = effectiveGain();
        if (targetGain != 1.0f || m_appliedGain != 1.0f) {
            const bool applied
                = applyGainRamp(buf.data(), got, m_devSpec.format, m_appliedGain, targetGain);
            if (applied) {
                m_appliedGain = targetGain;
            } else if (!m_gainFormatWarned) {
                // 违背约格式：宁可音量失效，也不把字节解释错。
                m_gainFormatWarned = true;
                LOG_WARNING(LOG_TAG, "volume gain skipped: unsupported device format %d",
                            static_cast<int>(m_devSpec.format));
            }
        }
        if (m_audioDev->write(buf.data(), got) < 0) {
            task->decoder->noteDeviceWriteFailure();
        }

        // 单一位置来源：解码器按「段基准 + ring 段内已读字节」换算。
        // 消费端只负责告知时间，不再累计自己的字节偏移（seek / 切歌时两套计数会漂移）。
        SignalProperties &signalProperties = cur->signalProperties;
        const uint64_t curPositionMs       = task->decoder->positionMs();
        signalProperties.curPositionMs     = curPositionMs;

        if (!isShuttingDown()) {
            const int trackIndex = static_cast<int>(cur->index);
            const uint64_t epoch  = m_positionEpoch.load();
            m_workQueue.asyncRunTask([this, curPositionMs, trackIndex, epoch]() {
                // 会话代际已变（seek / 换曲 / 重播）的迟到任务整体丢弃；
                // 旧曲的迟到任务也不得写进新曲会话。
                if (!isShuttingDown() && epoch == m_positionEpoch.load()
                    && trackIndex == m_positionTrackIndex.load()) {
                    reportPosition(curPositionMs, trackIndex, false, epoch);
                }
            });
        }
    }

    if (got == 0) {
        // 欠流一拍：DECODING 且非静默窗口（shouldFillSilence() 已在函数开头返回 false）。
        // 只有 0 字节才算；不足 4096 B 时照常送出，仍有声音（规格 §4.1）。
        if (task->decoder->state() == StreamDecoderState::DECODING) {
            task->decoder->noteUnderrun();
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
    // 新曲会话：递增代际、重置单调基准，位置从该曲的 curPositionMs（通常 0）重新起算。
    bumpPositionSession();
    reportPosition(property->signalProperties.curPositionMs,
                   static_cast<int>(property->index), true, m_positionEpoch.load());
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
        index.id    = property->fileProperties.musicId;
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
    // 契约（规格 §3 / #87）：原因先于结果，onError 在状态变化之前。
    if (m_listener != nullptr) {
        m_listener->onMusicPlayerError(code, detail, index, path, m_traceId);
    }
    // 规格 §6.3 / #90：与 handleDecodeError 统一，自动跳轨以 SkipTrack 为准；
    // 设备错误是 StopPlayback，不该靠跳轨绕过。
    const bool canAutoSkip = shouldAutoSkipOnError()
        && GetErrorInfo(code).action == ErrorAction::SkipTrack;
    if (!canAutoSkip) {
        // 不跳轨：停在当前处置结果上，发且仅发一次状态事件。
        updatePlayState(MusicPlayerState::StoppedState);
        return;
    }
    // autoSkip 成功时状态可能根本不变（切歌前后都是 Playing，不闪 Stopped）；
    // 只有跳轨失败才补那「一次状态事件」，避免先发一个不存在的状态。
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
            updatePlayState(MusicPlayerState::StoppedState);
        }
    });
}

void MusicPlayer::Impl::bumpPositionSession()
{
    m_positionEpoch.fetch_add(1);
    m_positionFloor.store(0);
}

void MusicPlayer::Impl::reportPosition(uint64_t positionMs, int trackIndex, bool resetFloor,
                                       uint64_t epoch)
{
    if (isShuttingDown() || m_listener == nullptr) {
        return;
    }
    // 会话代际已变（seek / 换曲 / 重播）的迟到任务整体丢弃，不能让它把新基准抬高。
    if (epoch != m_positionEpoch.load()) {
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
    // 去重键是「最近对外发出的状态」而非 m_state：初值 Stopped 从未发过，
    // 首播即失败 / skip 失败时仍要通知一次，不能静默停在 Stopped。
    if (m_stateNotified && m_lastReportedState == state) {
        return;
    }
    m_state.store(state);
    m_lastReportedState = state;
    m_stateNotified     = true;
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

void MusicPlayer::setVolume(int percent)
{
    m_impl->setVolume(percent);
}

int MusicPlayer::volume() const
{
    return m_impl->volume();
}

void MusicPlayer::setMuted(bool muted)
{
    m_impl->setMuted(muted);
}

bool MusicPlayer::muted() const
{
    return m_impl->muted();
}

size_t MusicPlayer::queuedBytesForTest() const
{
    return m_impl->queuedBytesForTest();
}

StreamMetrics MusicPlayer::metricsForTest() const
{
    return m_impl->metricsForTest();
}

bool MusicPlayer::retiredTaskExpiredForTest() const
{
    return m_impl->retiredTaskExpiredForTest();
}

bool MusicPlayer::setAudioDeviceForTest(std::shared_ptr<IAudioDevice> device)
{
    return m_impl->setAudioDeviceForTest(std::move(device));
}
}
