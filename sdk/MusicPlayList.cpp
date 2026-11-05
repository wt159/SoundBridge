#include "MusicPlayList.h"
#include "ExtractorFactory.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "Metrics.h"
#include "type_name.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <random>
#include <sstream>

namespace sdk {

#define LOG_TAG "MusicPlayList"

using namespace sdk_utils;

MusicPlayList::MusicPlayList(MusicPlayListCallback *callback, WorkQueue *wq, AudioSpec &devSpec)
    : m_callback(callback)
    , m_curIndex(0)
    , m_selectIndex(0)
    , m_workQueue(wq)
    , m_devSpec(devSpec)
    , m_musicListProperties()
    , m_playbackMode(MusicPlaybackMode::Sequential)
    , m_randomEngine(std::random_device {}())
{
    m_musicListProperties.clear();
    LOG_INFO(LOG_TAG, "MusicPlayList construct");
}

MusicPlayList::~MusicPlayList()
{
    // 析构是「join 必须由发起线程完成」的唯一例外（规格 4.4「析构例外」）：
    // 这里同样只 join、不 detach，销毁顺序是 decoder 先于 ring（见 PlaybackTask）。
    stopCurrentTask();
}

void MusicPlayList::addMusic(const std::string &musicPath)
{
    LOG_INFO(LOG_TAG, "addMusic : %s", musicPath.data());
    m_workQueue->asyncRunTask(&MusicPlayList::_addMusic, this, musicPath);
}
void MusicPlayList::addMusicWithNotify(const std::string &musicPath, std::function<void()> onDone)
{
    LOG_INFO(LOG_TAG, "addMusicWithNotify : %s", musicPath.data());
    m_workQueue->asyncRunTask([this, musicPath, onDone]() {
        _addMusic(musicPath);
        if (onDone) {
            onDone();
        }
    });
}
void MusicPlayList::next()
{
    m_workQueue->asyncRunTask([this]() { _next(); });
}
void MusicPlayList::pervious()
{
    m_workQueue->asyncRunTask([this]() { _pervious(); });
}
void MusicPlayList::setCurrentIndex(int index)
{
    m_workQueue->asyncRunTask([this, index]() { _setCurrentIndex(index); });
}
void MusicPlayList::updateList()
{
    m_workQueue->asyncRunTask(&MusicPlayList::_updateList, this);
}
int MusicPlayList::getMusicCount()
{
    return m_curIndex.load();
}

void MusicPlayList::setPlaybackMode(MusicPlaybackMode mode)
{
    m_playbackMode.store(mode);
}

MusicPlaybackMode MusicPlayList::playbackMode() const
{
    return m_playbackMode.load();
}

bool MusicPlayList::nextSync()
{
    if (m_curIndex.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    return selectTrack((m_selectMusicProperties->index + 1) % m_curIndex.load());
}

bool MusicPlayList::previousSync()
{
    if (m_curIndex.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    return selectTrack((m_selectMusicProperties->index + m_curIndex.load() - 1)
                       % m_curIndex.load());
}

bool MusicPlayList::setCurrentIndexSync(int index)
{
    if (m_curIndex.load() == 0) {
        return false;
    }
    if (index < 0 || static_cast<size_t>(index) >= m_musicListProperties.size()) {
        return false;
    }
    return selectTrack(static_cast<size_t>(index));
}

void MusicPlayList::setTraceId(const std::string &traceId)
{
    std::lock_guard<std::mutex> lock(m_traceMutex);
    m_traceId = traceId;
}

bool MusicPlayList::advanceToNextTrack()
{
    if (m_curIndex.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }

    const size_t count        = m_curIndex.load();
    const size_t currentIndex = m_selectMusicProperties->index;
    switch (m_playbackMode.load()) {
    case MusicPlaybackMode::CurrentItemOnce:
        return false;
    case MusicPlaybackMode::CurrentItemInLoop:
        return selectTrack(currentIndex);
    case MusicPlaybackMode::Sequential:
        for (size_t index = currentIndex + 1; index < count; ++index) {
            if (selectTrack(index)) {
                return true;
            }
        }
        return false;
    case MusicPlaybackMode::Loop:
        for (size_t step = 1; step <= count; ++step) {
            size_t index = (currentIndex + step) % count;
            if (selectTrack(index)) {
                return true;
            }
        }
        return false;
    case MusicPlaybackMode::Random: {
        std::vector<size_t> candidates;
        candidates.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            if (count > 1 && index == currentIndex) {
                continue;
            }
            candidates.push_back(index);
        }
        std::shuffle(candidates.begin(), candidates.end(), m_randomEngine);
        for (size_t index : candidates) {
            if (selectTrack(index)) {
                return true;
            }
        }
        return count == 1 ? selectTrack(currentIndex) : false;
    }
    }
    return false;
}

bool MusicPlayList::skipToNextPlayable()
{
    if (m_curIndex.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    size_t count      = m_curIndex.load();
    size_t startIndex = m_selectMusicProperties->index;
    // step 从 1 开始且最多 count-1：本曲已经在播/已失败，绕回它自己就是「同一曲重试第二次」，
    // 也会让两首坏曲互相跳过变成死循环（规格 8.2 / 实现清单 21）。
    for (size_t step = 1; step < count; ++step) {
        size_t index                 = (startIndex + step) % count;
        MusicPropertiesPtr candidate = m_musicListProperties.at(index);
        if (isTrackFailed(candidate->index)) {
            continue;
        }
        if (!replacePlaybackTask(candidate)) {
            markTrackFailed(candidate->index);
            continue;
        }
        resetTrackProgressExcept(candidate->index);
        candidate->signalProperties.curPositionMs = 0;
        m_selectMusicProperties                   = candidate;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
        return true;
    }
    return false;
}

void MusicPlayList::markTrackFailed(size_t index)
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    m_failedIndices.insert(index);
    LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, currentTraceId(),
                       "track marked failed in this session index=%zu", index);
}

bool MusicPlayList::isTrackFailed(size_t index) const
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    return m_failedIndices.find(index) != m_failedIndices.end();
}

void MusicPlayList::clearFailedTracks()
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    m_failedIndices.clear();
}

void MusicPlayList::recordScanRejection(ErrorCode code)
{
    std::lock_guard<std::mutex> lock(m_rejectMutex);
    m_scanRejects[code] += 1;
}

void MusicPlayList::reportScanSummary()
{
    std::map<ErrorCode, int> rejects;
    {
        std::lock_guard<std::mutex> lock(m_rejectMutex);
        rejects.swap(m_scanRejects);
    }
    if (rejects.empty()) {
        return;
    }
    for (std::map<ErrorCode, int>::const_iterator it = rejects.begin(); it != rejects.end(); ++it) {
        std::ostringstream detail;
        detail << it->second
               << ((it->second == 1) ? " file cannot be played" : " files cannot be played");
        std::string traceId = currentTraceId();
        LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, traceId,
                           "scan summary code=%d detail=%s", static_cast<int>(it->first),
                           detail.str().c_str());
        if (m_callback != nullptr) {
            m_callback->onMusicPlayListError(it->first, detail.str(), -1, "");
        }
    }
}
void MusicPlayList::_addMusic(const std::string &musicPath)
{
    auto openStart                       = std::chrono::steady_clock::now();
    MusicPropertiesPtr musicProperties   = std::make_shared<MusicProperties>();
    musicProperties->index               = m_curIndex;
    FileProperties &fileProperties       = musicProperties->fileProperties;
    SignalProperties &signalProperties   = musicProperties->signalProperties;
    ProcessProperties &processProperties = musicProperties->processProperties;
    fileProperties.fullPath              = musicPath;
    fileProperties.parseFileName();
    LOG_INFO(LOG_TAG, "fileName      : %s", fileProperties.fileName.data());
    LOG_INFO(LOG_TAG, "extensionName : %s", fileProperties.extensionName.data());
    std::shared_ptr<FileSource> source(new FileSource(musicPath.c_str()));
    if (source == nullptr) {
        recordScanRejection(ErrorCode::FileOpenFailed);
        reportError(ErrorCode::FileOpenFailed, "new FileSource failed", musicProperties);
        return;
    }
    if (source->initCheck() != OK) {
        recordScanRejection(ErrorCode::FileOpenFailed);
        reportError(ErrorCode::FileOpenFailed, "FileSource initCheck failed", musicProperties);
        return;
    }
    processProperties.source = source;
    // 规格 5.4 H3：拒绝的唯一出口是 createExtractor 返回空，错误码按「扩展名是否注册」分，
    // 不按「解析失败」分——注册过却被拒说明头坏了，没注册且魔数认不出才是格式不支持。
    const bool extensionRegistered
        = ExtractorFactory::isSupportedExtension(fileProperties.extensionName);
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(source.get(), fileProperties.extensionName, true));
    if (extractor == nullptr) {
        const ErrorCode code = extensionRegistered ? ErrorCode::ExtractorInitFailed
                                                   : ErrorCode::ExtractorUnsupported;
        const char *detail   = extensionRegistered ? "extractor rejected the file header"
                                                   : "createExtractor returned nullptr";
        recordScanRejection(code);
        reportError(code, detail, musicProperties);
        return;
    }
    auto openEnd = std::chrono::steady_clock::now();
    auto openMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(openEnd - openStart).count();
    Metrics::RecordTiming(
        "open_file_ms", static_cast<uint64_t>(openMs),
        MetricTags { currentTraceId(), musicPath, static_cast<int>(musicProperties->index) });
    processProperties.extractor    = extractor;
    signalProperties.curPositionMs = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    signalProperties.durationMs    = signalProperties.spec.durationMs;
    signalProperties.dataSize      = 0;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    m_musicListProperties.push_back(musicProperties);
    m_curIndex++;
    if (m_selectMusicProperties == nullptr) {
        // 只登记「当前曲目」：ring / decoder 是播放任务的资源，在 play 时创建。
        // 扫描阶段（包括给下一首做预缓冲）不持有任何播放资源（规格 4.3 所有权清单）。
        m_selectMusicProperties = musicProperties;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    }
    LOGI("this music fished, m_curIndex=%d", m_curIndex.load());
}

bool MusicPlayList::selectTrack(size_t index)
{
    if (m_curIndex.load() == 0 || index >= m_musicListProperties.size()) {
        return false;
    }

    MusicPropertiesPtr next = m_musicListProperties.at(index);
    // 切歌窗口：旧任务在新任务 start() 之前 stop+join 并销毁旧 ring，
    // 所以上一首的 PCM 不可能流进下一首（规格 4.1 切歌行）。
    if (!replacePlaybackTask(next)) {
        return false;
    }
    resetTrackProgressExcept(next->index);
    next->signalProperties.curPositionMs = 0;
    m_selectMusicProperties              = next;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    return true;
}

void MusicPlayList::_next()
{
    nextSync();
}
void MusicPlayList::_pervious()
{
    previousSync();
}
void MusicPlayList::_setCurrentIndex(int index)
{
    setCurrentIndexSync(index);
}
void MusicPlayList::_updateList()
{
    if (m_musicListProperties.size() > 0) {
        m_callback->updateMusicList(m_musicListProperties);
    }
}

PlaybackTaskPtr MusicPlayList::currentTask() const
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    return m_currentTask;
}

bool MusicPlayList::startCurrentTask()
{
    MusicPropertiesPtr current = m_selectMusicProperties;
    if (current == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        // 暂停后恢复、重复 play：同一个任务还活着就沿用，别把已经缓冲好的 PCM 扔掉。
        // 判据只认 MusicPlayerState 的投影（decoder 状态），不新增状态机。
        if (m_currentTask != nullptr && m_currentTask->trackIndex == current->index
            && m_currentTask->decoder != nullptr
            && m_currentTask->decoder->state() != StreamDecoderState::IDLE
            && m_currentTask->decoder->state() != StreamDecoderState::ERROR) {
            return true;
        }
    }
    return replacePlaybackTask(current);
}

void MusicPlayList::stopCurrentTask()
{
    PlaybackTaskPtr task;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        task.swap(m_currentTask);
    }
    if (task == nullptr) {
        return;
    }
    // 在这里 join：发起任务的控制线程自己收尾，禁止 detach / 后台 join。
    // 已写进 ring 的 PCM 不 drain——任务整体销毁，旧数据随 ring 一起消失。
    if (task->decoder != nullptr) {
        task->decoder->stop();
    }
    task->decoder.reset();
    task->ring.reset();
}

bool MusicPlayList::replacePlaybackTask(const MusicPropertiesPtr &musicProperties)
{
    if (musicProperties == nullptr) {
        return false;
    }
    ProcessProperties &processProperties = musicProperties->processProperties;
    if (processProperties.extractor == nullptr) {
        reportError(ErrorCode::ExtractorInitFailed, "extractor is nullptr", musicProperties);
        return false;
    }

    // 切歌开销：新任务 start() 之前销毁旧任务（join + 丢 ring）的那段时间。
    // 与 init 只读扫描耗时同构——只记录，不进首播判据、不设阈值（规格 4.3 / 11）。
    const auto switchStart = std::chrono::steady_clock::now();

    // 顺序不可交换：先 stop+join 旧任务、旧 ring 引用计数归零，再建新任务。
    stopCurrentTask();

    std::shared_ptr<AudioRingBuffer> ring
        = std::make_shared<AudioRingBuffer>(AudioRingBuffer::kDefaultCapacity);
    std::shared_ptr<AudioStreamDecoder> decoder
        = std::make_shared<AudioStreamDecoder>(ring.get(), m_devSpec);
    // start() 内部先 stop/join 自己的旧线程（本实例刚建，无旧线程），再起一个新解码线程。
    // ring 的 reset() 在新解码线程的第一件事里做（规格 4.4：只有解码线程能 reset）。
    sdk_utils::status_t ret = decoder->start(processProperties.extractor.get());
    if (ret != OK || decoder->state() == StreamDecoderState::ERROR) {
        decoder->stop();
        return false;
    }

    PlaybackTaskPtr task = std::make_shared<PlaybackTask>(ring, decoder, musicProperties->index);
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_currentTask = task;
    }

    const auto switchEnd = std::chrono::steady_clock::now();
    const auto switchMs  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              switchEnd - switchStart)
                              .count();
    Metrics::RecordTiming("track_switch_ms", static_cast<uint64_t>(switchMs),
                          MetricTags { currentTraceId(), musicProperties->fileProperties.fullPath,
                                       static_cast<int>(musicProperties->index) });
    return true;
}

void MusicPlayList::resetTrackProgressExcept(size_t keepIndex)
{
    for (auto &item : m_musicListProperties) {
        if (item == nullptr || item->index == keepIndex) {
            continue;
        }
        item->signalProperties.curPositionMs = 0;
        item->signalProperties.dataSize      = 0;
    }
}

void MusicPlayList::reportError(ErrorCode code, const std::string &detail,
                                const MusicPropertiesPtr &musicProperties)
{
    int index = -1;
    std::string path;
    if (musicProperties != nullptr) {
        index = static_cast<int>(musicProperties->index);
        path  = musicProperties->fileProperties.fullPath;
    }
    std::string traceId = currentTraceId();
    LogPrintfWithTrace(SdkLogLevel::Error, LOG_TAG, traceId, "error=%d detail=%s index=%d path=%s",
                       static_cast<int>(code), detail.c_str(), index, path.c_str());
    bool shouldNotify
        = (m_selectMusicProperties == nullptr || musicProperties == m_selectMusicProperties);
    if (m_callback != nullptr && shouldNotify) {
        m_callback->onMusicPlayListError(code, detail, index, path);
    }
}

std::string MusicPlayList::currentTraceId() const
{
    std::lock_guard<std::mutex> lock(m_traceMutex);
    return m_traceId;
}
}
