#include "MusicPlayList.h"
#include "ExtractorFactory.h"
#include "FileSearch.h"
#include "HttpSource.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "Metrics.h"
#include "UrlSource.h"
#include "Utf8Path.h"
#include "type_name.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cwctype>
#include <deque>
#include <random>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
// windows.h（经 Utf8Path.h 进来）把 ERROR 定义成宏（0），会打断本文件里
// StreamDecoderState::ERROR 的枚举名。与 sdk/test/mocks/HttpMocks.hpp 同一处理：
// push 后 undef，本 TU 内不再需要这个宏。
#pragma push_macro("ERROR")
#undef ERROR
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sdk {

#define LOG_TAG "MusicPlayList"

using namespace sdk_utils;

namespace {

// 打开失败的分级文案（#278）：错误码面维持既有 FileOpenFailed，成因只落文本。
inline const char *openFailureDetailForStatus(status_t status)
{
    switch (status) {
    case NO_MEMORY:
        return "open failed: too many open files";
    case PERMISSION_DENIED:
        return "open failed: permission denied";
    case NO_INIT:
        return "open failed: not found";
    default:
        return "open failed";
    }
}

// 句柄预算的两个 L2 gauge（observability-contract.md §5.1）：只在扫描汇总 / 切歌完成
// 两个边界记录，避免千曲扫描产生 O(n) 条仪表日志。
inline void recordHandleGauges(const MetricTags &tags)
{
    Metrics::RecordGauge("datasource_open_handles",
                         static_cast<double>(FileSource::openHandleCount()), tags, "count");
    Metrics::RecordGauge("datasource_open_peak",
                         static_cast<double>(FileSource::peakOpenHandleCount()), tags, "count");
}

inline bool looksLikeHttpUrl(const std::string &path)
{
    return path.compare(0, 7, "http://") == 0 || path.compare(0, 8, "https://") == 0;
}

inline bool isSeparator(wchar_t ch)
{
    return ch == L'/' || ch == L'\\';
}

inline bool isSeparator(char ch)
{
    return ch == '/'
#ifdef _WIN32
        || ch == '\\'
#endif
        ;
}

// 归一化的公共尾段：按分隔符切分 → 消 `.` / `..` → 用 `/` 重连。
// 前缀（前导分隔符 / UNC 双分隔符）由调用方保留在 `prefix` 里。
template <typename CharT, typename SepFn>
std::basic_string<CharT> normalizeSegments(const std::basic_string<CharT> &path, SepFn isSep,
                                           const std::basic_string<CharT> &prefix)
{
    std::vector<std::basic_string<CharT>> segments;
    std::basic_string<CharT> current;
    for (CharT ch : path) {
        if (isSep(ch)) {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        segments.push_back(current);
    }

    std::vector<std::basic_string<CharT>> normalized;
    for (const std::basic_string<CharT> &segment : segments) {
        if (segment.size() == 1 && segment[0] == static_cast<CharT>('.')) {
            continue;
        }
        if (segment.size() == 2 && segment[0] == static_cast<CharT>('.')
            && segment[1] == static_cast<CharT>('.')) {
            const bool backIsParent = !normalized.empty() && normalized.back().size() == 2
                                   && normalized.back()[0] == static_cast<CharT>('.')
                                   && normalized.back()[1] == static_cast<CharT>('.');
            if (!normalized.empty() && !backIsParent) {
                normalized.pop_back();
            } else if (prefix.empty()) {
                // 相对路径（兜底）：`..` 保留。实际调用前都已绝对化，这里只是兜底。
                normalized.push_back(segment);
            }
            // 绝对路径且已在根：`..` 越根丢弃，与 lexically_normal 一致。
            continue;
        }
        normalized.push_back(segment);
    }

    std::basic_string<CharT> joined = prefix;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i != 0) {
            joined.push_back(static_cast<CharT>('/'));
        }
        joined += normalized[i];
    }
    return joined;
}

template <typename CharT>
std::basic_string<CharT> leadingPrefix(const std::basic_string<CharT> &path)
{
    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1])) {
        // UNC（`\\server\share`）与 POSIX 的双斜杠前缀都按两个分隔符保留。
        return std::basic_string<CharT>(2, static_cast<CharT>('/'));
    }
    if (!path.empty() && isSeparator(path[0])) {
        return std::basic_string<CharT>(1, static_cast<CharT>('/'));
    }
    return std::basic_string<CharT>();
}

// `addTrack` 的「传目录即丢弃」判据（契约 §4.1）。必须在队列线程调用。
// Windows 走 `_wstat`：路径可能含非 ASCII，窄字符 API 会按 CP_ACP 解释而失效。
bool pathIsDirectory(const std::string &path)
{
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    const std::wstring wide = sdk_utils::toWideString(path.c_str());
    if (wide.empty()) {
        return false;
    }
    struct _stat info;
    if (_wstat(wide.c_str(), &info) != 0) {
        return false;
    }
    return (info.st_mode & _S_IFDIR) != 0;
#else
    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISDIR(info.st_mode);
#endif
}

} // namespace

// pending 切歌的请求状态（ADR 0020）。IO 线程只写 lease / status 两个字段，
// 控制线程在 commit 时读；两者之间由 runBounded 的完成标志或 WorkQueue 回投
// 任务建立 happens-before（互斥量与队列锁）。
struct MusicPlayList::PendingSwitch {
    uint64_t generation = 0;
    uint64_t uid        = 0;
    MusicPropertiesPtr properties;
    std::shared_ptr<SourceLease> lease;
    sdk_utils::status_t status = sdk_utils::UNKNOWN_ERROR;
    SwitchOrigin origin        = SwitchOrigin::User;
    // 自动跳过 / 段末前进的剩余候选（order 是候选下标序，nextCandidate 是下一个要试的）。
    std::vector<size_t> order;
    size_t nextCandidate = 0;
    std::chrono::steady_clock::time_point begin;
    // 指标标签在提交时抄下来：等待期 traceId 可能已被后续命令刷新。
    std::string traceId;
    std::string path;
    int index = -1;
};

// 一次探测的自包含结果（ADR 0021 决策 5）：扫描线程产出，控制线程消费。
// 探测阶段只碰文件系统与 ExtractorFactory，列表状态（index / uid / 计数 /
// 首曲登记 / 回调 / 拒绝记账）全部留到插入时做。
struct MusicPlayList::ScanProbeOutcome {
    std::string path;
    MusicPropertiesPtr properties;
    bool ok = false;
    ErrorCode code = ErrorCode::FileOpenFailed;
    std::string detail;
    // 打开失败的成因（#278）；只在 initCheck 失败时非空，其余失败不记成因。
    std::string cause;
    uint64_t openMs = 0;
};

// 扫描请求（ADR 0021 决策 6）：每次 scanDirectory 生成一个。请求指针只在控制
// 线程读写；扫描线程只碰请求自身，不碰列表成员。
struct MusicPlayList::ScanRequest {
    // 请求代际（ADR 0021 决策 6）：控制线程在插入前用它判定「仍是当前请求」。
    uint64_t generation;
    std::string rootPath;
    // 控制线程置位；扫描线程在**每个文件边界**检查一次（决策 6）。
    std::atomic<bool> cancelled;
    // 已产出、尚未处理的结果数：归零且遍历结束 ⇒ 汇总点。
    std::atomic<int> pending;
    // 保护 ready / scanFinished，并承载背压等待（决策 3）。
    std::mutex mutex;
    std::condition_variable space;
    // 已回投、待控制线程按到达序插入的结果。用队列而不是「把结果塞进投递任务」，
    // 是因为 WorkQueue 是同刻时间戳无序的优先队列——串行产出 + 队列保序让
    // 「到达序 == 扫描序」不依赖投递任务本身的调度次序。
    // **ready.size() 就是决策 3 的「已回投未插入」条数**（outstandingInserts）：
    // 不再另放一个原子镜像，避免两处真源分叉；读它一律持 mutex。
    std::deque<ScanProbeOutcome> ready;
    bool scanFinished = false;
    // 汇总只发一次（只在控制线程读写）。
    bool summaryReported = false;
    // 请求级拒绝计数（ADR 0021 决策 6）：随请求一起丢弃，不会串到下一批。
    // 控制线程写；测试线程可能直接调 reportScanSummary()，用锁兜住。
    std::mutex rejectMutex;
    std::map<ErrorCode, int> rejects;
    std::map<std::string, int> causes;

    ScanRequest()
        : generation(0)
        , cancelled(false)
        , pending(0)
    {
    }
};

std::string deriveMusicId(const std::string &path)
{
    if (path.empty()) {
        return std::string();
    }
    if (looksLikeHttpUrl(path)) {
        // 网络 URL 的"同一资源"由 URL 本身定义，任何归一化都可能改变语义（大小写敏感的
        // path 段、query 顺序），因此原样返回。
        return path;
    }

#ifdef _WIN32
    // Windows 上必须走宽字符：路径可能含非 ASCII，窄字符小写折叠会破坏字节。
    std::wstring wide = sdk_utils::toWideString(path.c_str());
    if (wide.empty()) {
        return path;
    }
    const bool absolute = (wide.size() >= 3 && wide[1] == L':' && isSeparator(wide[2]))
                       || (!wide.empty() && isSeparator(wide[0]));
    if (!absolute) {
        wchar_t cwdBuffer[4096];
        if (_wgetcwd(cwdBuffer, static_cast<int>(sizeof(cwdBuffer) / sizeof(wchar_t))) != nullptr) {
            wide = std::wstring(cwdBuffer) + L"/" + wide;
        }
    }
    // 盘符必须并进前缀：留在段里的话，越根的 `..` 会把 `c:` 当成可回退的目录弹掉，
    // `C:/a/../../b` 会归一成 `b`（先前的用例只测到不越根的路径，掩盖了这个缺口）。
    std::wstring prefix;
    std::wstring body = wide;
    if (wide.size() >= 3 && wide[1] == L':' && isSeparator(wide[2])) {
        prefix = wide.substr(0, 2);
        body   = wide.substr(2);
    }
    prefix += leadingPrefix(body);
    std::wstring normalized
        = normalizeSegments(body, [](wchar_t ch) { return ch == L'/' || ch == L'\\'; }, prefix);
    for (wchar_t &ch : normalized) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return sdk_utils::toUtf8String(boost::filesystem::path(normalized));
#else
    std::string narrow = path;
    if (narrow[0] != '/') {
        char cwdBuffer[4096];
        if (getcwd(cwdBuffer, sizeof(cwdBuffer)) != nullptr) {
            narrow = std::string(cwdBuffer) + "/" + narrow;
        }
    }
    // POSIX 区分大小写，不做折叠。
    return normalizeSegments(narrow, [](char ch) { return ch == '/'; }, leadingPrefix(narrow));
#endif
}

MusicPlayList::MusicPlayList(MusicPlayListCallback *callback, WorkQueue *wq, AudioSpec &devSpec)
    : m_callback(callback)
    , m_count(0)
    , m_workQueue(wq)
    , m_devSpec(devSpec)
    , m_musicListProperties()
    , m_playbackMode(MusicPlaybackMode::Sequential)
    , m_nextUid(1)
    , m_randomEngine(std::random_device {}())
    , m_openExecutor(new SwitchOpenExecutor())
    , m_scanExecutor(new ScanExecutor())
    , m_switchPending(false)
    , m_switchGeneration(0)
{
    m_musicListProperties.clear();
    LOG_INFO(LOG_TAG, "MusicPlayList construct");
}

MusicPlayList::~MusicPlayList()
{
    // ADR 0021 决策 7：先置取消位让扫描线程在每个文件边界退出，再有界等待 200 ms；
    // 卡在 open() / read() 里的那一次不可取消，超时 detach（与 ADR 0020 决策 5 同依据）。
    // 必须先于下面的 stopCurrentTask：扫描线程只回投结果，不碰任务槽。
    cancelScan();
    if (m_scanExecutor != nullptr) {
        m_scanExecutor->shutdown(std::chrono::milliseconds(200));
    }
    // ADR 0020 决策 5：先作废在途切换（结果回投后会被弱引用丢弃），再给执行器
    // 最多 200 ms 收尾；仍卡在 open() 里则 detach。**不允许**为了「无线程泄漏」
    // 改成无限 join——退出会被 SMB 60 s / NFS 无限重试顶住。
    // notify=false：回调对象可能已半析构，只作废不通知。
    cancelPendingSwitch(false);
    if (m_openExecutor != nullptr) {
        m_openExecutor->shutdown(std::chrono::milliseconds(200));
    }
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

// 目录扫描（ADR 0021 决策 1/2/3/5/6）：调用线程（生产路径 = UI 线程）只做三件事：
// 取消旧请求、建新请求、提交。遍历与探测全部在扫描线程上。
void MusicPlayList::scanDirectory(const std::string &rootPath)
{
    LOG_INFO(LOG_TAG, "scanDirectory : %s", rootPath.data());
    // 后者胜（决策 6）：与 ADR 0020 决策 3 对 next / previous 的处置同形。旧请求的
    // 迟到结果在 onScanResultsReady 里按身份丢弃，不产生任何回调。
    cancelScan();

    std::shared_ptr<ScanRequest> request = std::make_shared<ScanRequest>();
    request->rootPath                    = rootPath;
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        request->generation = ++m_scanGeneration;
        m_scanRequest       = request;
    }

    std::weak_ptr<MusicPlayList> weakSelf = shared_from_this();
    m_scanExecutor->submit([weakSelf, request]() { runScanRequest(weakSelf, request); });
}

// 取消在途扫描（决策 6）：只置位 + 唤醒背压等待，不触发汇总、不产生回调。
// 幂等；没有在途请求时是空操作。
void MusicPlayList::cancelScan()
{
    std::shared_ptr<ScanRequest> request;
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        request.swap(m_scanRequest);
    }
    if (request == nullptr) {
        return;
    }
    request->cancelled.store(true);
    request->space.notify_all();
}

bool MusicPlayList::isCurrentScan(const std::shared_ptr<ScanRequest> &request) const
{
    std::lock_guard<std::mutex> lock(m_scanMutex);
    return m_scanRequest != nullptr && m_scanRequest->generation == request->generation;
}

std::shared_ptr<MusicPlayList::ScanRequest> MusicPlayList::currentScan() const
{
    std::lock_guard<std::mutex> lock(m_scanMutex);
    return m_scanRequest;
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
void MusicPlayList::addTrack(const std::string &musicPath)
{
    LOG_INFO(LOG_TAG, "addTrack : %s", musicPath.data());
    m_workQueue->asyncRunTask(&MusicPlayList::_addTrack, this, musicPath);
}
void MusicPlayList::removeTrack(int index)
{
    LOG_INFO(LOG_TAG, "removeTrack : %d", index);
    m_workQueue->asyncRunTask(&MusicPlayList::_removeTrack, this, index);
}
void MusicPlayList::clearPlaylist()
{
    LOG_INFO(LOG_TAG, "clearPlaylist");
    m_workQueue->asyncRunTask(&MusicPlayList::_clearPlaylist, this);
}
void MusicPlayList::updateList()
{
    m_workQueue->asyncRunTask(&MusicPlayList::_updateList, this);
}
int MusicPlayList::getMusicCount()
{
    return m_count.load();
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
    if (m_count.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    return selectTrack((m_selectMusicProperties->index + 1) % m_count.load());
}

bool MusicPlayList::previousSync()
{
    if (m_count.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    return selectTrack((m_selectMusicProperties->index + m_count.load() - 1)
                       % m_count.load());
}

bool MusicPlayList::setCurrentIndexSync(int index)
{
    if (m_count.load() == 0) {
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
    if (m_count.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }

    const size_t count        = m_count.load();
    const size_t currentIndex = m_selectMusicProperties->index;
    // 段末前进与自动跳过共用同一个候选循环（ADR 0020）：Pending 也算「已接受」，
    // 失败时由 settlePendingFailure 从剩余候选继续。
    std::vector<size_t> order;
    switch (m_playbackMode.load()) {
    case MusicPlaybackMode::CurrentItemOnce:
        return false;
    case MusicPlaybackMode::CurrentItemInLoop:
        order.push_back(currentIndex);
        break;
    case MusicPlaybackMode::Sequential:
        for (size_t index = currentIndex + 1; index < count; ++index) {
            order.push_back(index);
        }
        break;
    case MusicPlaybackMode::Loop:
        for (size_t step = 1; step <= count; ++step) {
            order.push_back((currentIndex + step) % count);
        }
        break;
    case MusicPlaybackMode::Random: {
        for (size_t index = 0; index < count; ++index) {
            if (count > 1 && index == currentIndex) {
                continue;
            }
            order.push_back(index);
        }
        std::shuffle(order.begin(), order.end(), m_randomEngine);
        if (order.empty() && count == 1) {
            order.push_back(currentIndex);
        }
        break;
    }
    }
    return tryCandidates(order, 0, SwitchOrigin::EndOfStream);
}

bool MusicPlayList::skipToNextPlayable()
{
    if (m_count.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    const size_t count      = m_musicListProperties.size();
    const size_t startIndex = m_selectMusicProperties->index;
    // step 从 1 开始且最多 count-1：本曲已经在播/已失败，绕回它自己就是「同一曲重试第二次」，
    // 也会让两首坏曲互相跳过变成死循环（规格 8.2 / 实现清单 21）。
    std::vector<size_t> order;
    order.reserve(count > 0 ? count - 1 : 0);
    for (size_t step = 1; step < count; ++step) {
        order.push_back((startIndex + step) % count);
    }
    // 失败终态由调用方决定（解码错误落 Paused、设备错误落 Stopped），候选循环本身
    // 只负责接受意图与续做。
    return selectFirstPlayable(order, SwitchOrigin::AutoSkip);
}

std::vector<size_t> MusicPlayList::seatingOrder(size_t p)
{
    const size_t count = m_musicListProperties.size();
    std::vector<size_t> order;
    if (count == 0) {
        return order;
    }
    order.reserve(count);
    if (m_playbackMode.load() == MusicPlaybackMode::Random) {
        for (size_t index = 0; index < count; ++index) {
            order.push_back(index);
        }
        std::shuffle(order.begin(), order.end(), m_randomEngine);
        return order;
    }
    // Sequential / Loop / 单曲模式：从删除点起的环形顺序。单曲模式在补位语境下
    // 退化为顺序补位序——「尾部即止」约束的是自动前进，不是「必须有当前曲目」。
    for (size_t step = 0; step < count; ++step) {
        order.push_back((p + step) % count);
    }
    return order;
}

bool MusicPlayList::selectFirstPlayable(const std::vector<size_t> &order, SwitchOrigin origin)
{
    return tryCandidates(order, 0, origin);
}

bool MusicPlayList::tryCandidates(const std::vector<size_t> &order, size_t from,
                                  SwitchOrigin origin)
{
    const size_t count = m_musicListProperties.size();
    for (size_t i = from; i < order.size(); ++i) {
        const size_t index = order.at(i);
        if (index >= count) {
            continue; // 防御：候选序由 seatingOrder 生成，正常情况下必然在范围内
        }
        MusicPropertiesPtr candidate = m_musicListProperties.at(index);
        if (isTrackFailed(candidate->uid)) {
            continue;
        }
        const SwitchStart start = beginSwitch(candidate, origin);
        if (start == SwitchStart::Failed) {
            markTrackFailed(candidate->uid);
            continue;
        }
        if (start == SwitchStart::Pending) {
            // 等待期失败后从下一个候选继续（ADR 0020 决策 4）；一次用户命令
            // 只产生一个最终结果，中间候选不产生可见的切换事件。
            if (m_pendingSwitch != nullptr) {
                m_pendingSwitch->order         = order;
                m_pendingSwitch->nextCandidate = i + 1;
            }
            return true;
        }
        publishSelection(candidate);
        return true;
    }
    return false;
}

void MusicPlayList::publishPlaylist()
{
    m_callback->updateMusicList(m_musicListProperties);
}

void MusicPlayList::markTrackFailed(uint64_t uid)
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    m_failedUids.insert(uid);
    LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, currentTraceId(),
                       "track marked failed in this session uid=%llu",
                       static_cast<unsigned long long>(uid));
}

bool MusicPlayList::isTrackFailed(uint64_t uid) const
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    return m_failedUids.find(uid) != m_failedUids.end();
}

void MusicPlayList::clearFailedTracks()
{
    std::lock_guard<std::mutex> lock(m_failedMutex);
    m_failedUids.clear();
}

uint64_t MusicPlayList::trackUidForTest(size_t index) const
{
    if (index >= m_musicListProperties.size() || m_musicListProperties[index] == nullptr) {
        return 0;
    }
    return m_musicListProperties[index]->uid;
}

std::weak_ptr<MusicProperties> MusicPlayList::trackRefForTest(size_t index) const
{
    if (index >= m_musicListProperties.size()) {
        return std::weak_ptr<MusicProperties>();
    }
    return std::weak_ptr<MusicProperties>(m_musicListProperties[index]);
}

void MusicPlayList::recordScanRejection(const std::shared_ptr<ScanRequest> &request,
                                        ErrorCode code)
{
    if (request == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(request->rejectMutex);
    request->rejects[code] += 1;
}

// 公开入口（测试用来断言「拒绝累计不外溢」）：报当前请求的汇总。没有在途请求时
// 只记录句柄 gauge，不产生任何回调。
void MusicPlayList::reportScanSummary()
{
    std::shared_ptr<ScanRequest> request = currentScan();
    if (request == nullptr) {
        recordHandleGauges(MetricTags(currentTraceId(), std::string(), -1));
        return;
    }
    reportScanSummaryFor(request);
}

void MusicPlayList::reportScanSummaryFor(const std::shared_ptr<ScanRequest> &request)
{
    std::map<ErrorCode, int> rejects;
    std::map<std::string, int> causes;
    {
        std::lock_guard<std::mutex> lock(request->rejectMutex);
        rejects.swap(request->rejects);
        causes.swap(request->causes);
    }
    // 句柄预算（observability-contract.md §5.1，L2 只记录不设阈值）：
    // 挂在这个边界上，千曲扫描不会产生 O(n) 条仪表日志。
    recordHandleGauges(MetricTags(currentTraceId(), std::string(), -1));
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
    // 成因拆分是**追加**的一条（#278）：既有 code=N detail=… 那行与聚合键一字不动。
    if (!causes.empty()) {
        std::ostringstream line;
        for (std::map<std::string, int>::const_iterator it = causes.begin(); it != causes.end();
             ++it) {
            if (it != causes.begin()) {
                line << " ";
            }
            line << it->first << "=" << it->second;
        }
        LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, currentTraceId(),
                           "scan summary causes %s", line.str().c_str());
    }
}

void MusicPlayList::recordScanRejectCause(const std::shared_ptr<ScanRequest> &request,
                                          const char *cause)
{
    if (request == nullptr || cause == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(request->rejectMutex);
    request->causes[cause] += 1;
}
void MusicPlayList::_addMusic(const std::string &musicPath)
{
    addMusicInternal(musicPath, false);
}

// 一次探测（ADR 0021 决策 5）：只读文件系统与 ExtractorFactory，产出**自包含**结果。
// 不碰 m_musicListProperties / m_count / m_nextUid / m_callback——它们没有互斥量，
// 只能在控制线程写。失败也只回填错误码 / detail / 成因，上报留在插入时做。
MusicPlayList::ScanProbeOutcome MusicPlayList::probeMusicFile(const std::string &musicPath)
{
    ScanProbeOutcome outcome;
    outcome.path                         = musicPath;
    auto openStart                       = std::chrono::steady_clock::now();
    MusicPropertiesPtr musicProperties   = std::make_shared<MusicProperties>();
    outcome.properties                   = musicProperties;
    FileProperties &fileProperties       = musicProperties->fileProperties;
    SignalProperties &signalProperties   = musicProperties->signalProperties;
    ProcessProperties &processProperties = musicProperties->processProperties;
    fileProperties.fullPath              = musicPath;
    // 身份键的唯一产生点（ADR 0014）：addMusic / addMusicWithNotify / URL 直通三条入口
    // 都经过这里，归一化只算这一次，下游（MusicIndex → TrackInfo::id）原样传递。
    fileProperties.musicId               = deriveMusicId(musicPath);
    fileProperties.parseFileName();
    LOG_INFO(LOG_TAG, "fileName      : %s", fileProperties.fileName.data());
    LOG_INFO(LOG_TAG, "extensionName : %s", fileProperties.extensionName.data());
    std::shared_ptr<DataSource> source;
    // P2-1→P3（issue #144/#152 / 规格 §9）：http(s) 分派加一级清单内容判定。
    // .m3u8 与其余 http URL 由 UrlSource 构造期一次性头部嗅探做内容层判定：
    //   VOD 特征（ENDLIST / PLAYLIST-TYPE:VOD）→ UrlSource 直通路径（现状不动）；
    //   LIVE 正面特征（#EXTM3U + 无 ENDLIST/VOD + 至少一条分片/variant 行，
    //   EVENT 归 LIVE，D17/D18）→ UrlSource 直通路径放行，durationMs==0；
    //   伪清单 / 坏清单 → 分派失败明确拒绝，不回退；
    //   嗅探失败 / 非清单内容 → P0 二选一（HttpSource 探测层），探测命中 Icy-*
    //   时改走原生 URL 层直通（D19：明确拒绝自定义 AVIO 路径，不静默回退）。
    // .mpd（P4 未重开）与 https:// 维持 P0 网络外拒绝（requestCount()==0 判据不动）。
    std::string extractorExtension = fileProperties.extensionName;
    const bool isHttp = musicPath.rfind("http://", 0) == 0;
    const bool isHttps = musicPath.rfind("https://", 0) == 0;
    if (isHttp && !UrlSource::isMpdExtensionUrl(musicPath)) {
        UrlSource *urlSource = new UrlSource(musicPath.c_str());
        if (urlSource->initCheck() == OK && urlSource->headLooksLikePlaylist()) {
            if (urlSource->headHasVodFeatures() || urlSource->headLooksLikeLivePlaylist()) {
                source.reset(urlSource);
                // 清单内容已确认为 m3u8（VOD 或 LIVE/EVENT 直通）：按内容走注册表，
                // 不用 URL 上的扩展名；LIVE 的无限流语义由解码线程状态机承接。
                extractorExtension = "m3u8";
            } else {
                // 伪清单 / 坏清单（#EXTM3U 起始但不满足 VOD 或 LIVE 正面特征集）：
                // 明确拒绝不回退（规格 §9.1：防止空清单无限重试）。
                delete urlSource;
                outcome.code   = ErrorCode::FileOpenFailed;
                outcome.detail = "playlist content has neither VOD nor LIVE features";
                return outcome;
            }
        } else {
            delete urlSource;
            HttpSource *httpSource = new HttpSource(musicPath.c_str());
            if (httpSource->initCheck() != OK && httpSource->icyDetected()) {
                // P3 ICY（#152 D19）：探测命中 Icy-* → 这是无限直播流，明确拒绝
                // 自定义 AVIO 路径（要求已知长度 + Range/206），改走原生 URL 层
                // 直通提取器（avformat_open_input 自动探测 demuxer，复用同一
                // 诊断旁路），不静默回退。
                delete httpSource;
                UrlSource *icySource = new UrlSource(musicPath.c_str());
                if (icySource->initCheck() == OK) {
                    source.reset(icySource);
                    // 按内容映射注册键：ICY 不带 .m3u8 扩展名，直通提取器靠
                    // 自动探测承载（HlsExtractor 的 P3 直通职能）。
                    extractorExtension = "m3u8";
                } else {
                    delete icySource;
                    outcome.code   = ErrorCode::FileOpenFailed;
                    outcome.detail = "icy source native url open failed";
                    return outcome;
                }
            } else {
                source.reset(httpSource);
            }
        }
    } else if (isHttp || isHttps) {
        source.reset(new HttpSource(musicPath.c_str()));
    } else {
        source.reset(new FileSource(musicPath.c_str()));
    }
    if (source == nullptr) {
        outcome.code   = ErrorCode::FileOpenFailed;
        outcome.detail = "new DataSource failed";
        return outcome;
    }
    if (source->initCheck() != OK) {
        // 成因分级（#278）：错误码仍是 FileOpenFailed，detail 区分句柄耗尽 / 权限 / 找不到。
        const status_t openStatus = source->openStatus();
        const char *detail        = openFailureDetailForStatus(openStatus);
        outcome.code              = ErrorCode::FileOpenFailed;
        outcome.detail            = detail;
        outcome.cause             = detail;
        return outcome;
    }
    processProperties.source = source;
    // 规格 5.4 H3：拒绝的唯一出口是 createExtractor 返回空，错误码按「扩展名是否注册」分，
    // 不按「解析失败」分——注册过却被拒说明头坏了，没注册且魔数认不出才是格式不支持。
    // P2-1 直通路径按清单内容判定的扩展名（extractorExtension）参与同一出口。
    const bool extensionRegistered
        = ExtractorFactory::isSupportedExtension(extractorExtension);
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(source.get(), extractorExtension, true));
    if (extractor == nullptr) {
        const ErrorCode code = extensionRegistered ? ErrorCode::ExtractorInitFailed
                                                   : ErrorCode::ExtractorUnsupported;
        const char *detail   = extensionRegistered ? "extractor rejected the file header"
                                                   : "createExtractor returned nullptr";
        // 探测失败也要归还句柄：条目不会入列，宁可这里显式释放。
        processProperties.source->release();
        outcome.code   = code;
        outcome.detail = detail;
        return outcome;
    }
    auto openEnd = std::chrono::steady_clock::now();
    outcome.openMs
        = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(openEnd
                                                                                     - openStart)
                                    .count());
    processProperties.extractor    = extractor;
    signalProperties.curPositionMs = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    signalProperties.durationMs    = signalProperties.spec.durationMs;
    signalProperties.dataSize      = 0;
    // 探测结束即释放构造租约（streaming-playback-spec.md §4.5）：条目只留数据源对象、
    // extractor 与已解析元数据，常驻句柄数不再随条目数增长。播放前由 PlaybackTask 重开。
    processProperties.source->release();
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    outcome.ok = true;
    return outcome;
}

// 把一次探测结果落成列表状态（ADR 0021 决策 5）：index / uid / push_back / 计数 /
// 首曲登记 / 拒绝记账 / 上报全部在这里，**只在控制线程执行**。
MusicPropertiesPtr MusicPlayList::insertProbedTrack(const ScanProbeOutcome &outcome,
                                                    bool userInitiated,
                                                    const std::shared_ptr<ScanRequest> &request)
{
    if (outcome.properties == nullptr) {
        return nullptr;
    }
    // index 在插入时才定：扫描线程只产出条目，位置由控制线程按到达序赋。失败条目也带
    // 上当时的 size——旧路径在探测前取 size，控制队列串行使两者恒等，日志与回调不变。
    outcome.properties->index = m_musicListProperties.size();
    if (!outcome.ok) {
        if (!userInitiated) {
            recordScanRejection(request, outcome.code);
            if (!outcome.cause.empty()) {
                recordScanRejectCause(request, outcome.cause.c_str());
            }
        }
        reportError(outcome.code, outcome.detail, outcome.properties, userInitiated);
        return nullptr;
    }
    MusicPropertiesPtr musicProperties = outcome.properties;
    Metrics::RecordTiming(
        "open_file_ms", outcome.openMs,
        MetricTags { currentTraceId(), outcome.path, static_cast<int>(musicProperties->index) });
    musicProperties->uid = m_nextUid++;
    m_musicListProperties.push_back(musicProperties);
    // 计数是 size() 的镜像：删除会压缩重排，自增语义在这里必然失效（ADR 0018 §1）。
    m_count.store(m_musicListProperties.size());
    if (!userInitiated && m_selectMusicProperties == nullptr) {
        // 只登记「当前曲目」：ring / decoder 是播放任务的资源，在 play 时创建。
        // 扫描阶段（包括给下一首做预缓冲）不持有任何播放资源（规格 4.3 所有权清单）。
        m_selectMusicProperties = musicProperties;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    }
    LOGI("this music fished, m_count=%d", static_cast<int>(m_count.load()));
    return musicProperties;
}

MusicPropertiesPtr MusicPlayList::addMusicInternal(const std::string &musicPath, bool userInitiated)
{
    return insertProbedTrack(probeMusicFile(musicPath), userInitiated, nullptr);
}

// 扫描线程主体（ADR 0021 决策 1/3/6）：遍历与逐文件探测顺序执行。只碰 request 与
// 自由函数；weakSelf 只用于回投，目标析构后 lock 失败即静默返回。
void MusicPlayList::runScanRequest(const std::weak_ptr<MusicPlayList> &weakSelf,
                                   const std::shared_ptr<ScanRequest> &request)
{
    const std::vector<std::string> paths = recursiveFileSearch(request->rootPath);
    for (size_t i = 0; i < paths.size(); ++i) {
        // 取消检查在**每个文件边界**（决策 6）：卡在 _wfopen / read 里的那一次
        // 不可取消，最多延迟到它返回。
        if (request->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        ScanProbeOutcome outcome = probeMusicFile(paths[i]);
        {
            std::unique_lock<std::mutex> lock(request->mutex);
            // 背压（决策 3）：控制队列里「已回投未插入」的条数到上限就等，防止千条
            // 结果一次性涌入把 10 ms 消费节拍挤在后面。
            request->space.wait(lock, [&request]() {
                return request->cancelled.load(std::memory_order_acquire)
                    || request->ready.size() < kScanInsertOutstandingMax;
            });
            if (request->cancelled.load(std::memory_order_acquire)) {
                return;
            }
            request->ready.push_back(std::move(outcome));
            request->pending.fetch_add(1, std::memory_order_release);
        }
        postScanResultsReady(weakSelf, request);
    }
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->scanFinished = true;
    }
    // 末条结果可能在上一次唤醒里已处理完：补一次唤醒，保证汇总点不被错过。
    postScanResultsReady(weakSelf, request);
}

// 把「有结果可取了」投回控制队列（扫描线程调用）。目标存活 ⇒ 其 WorkQueue 也存活
//（Impl 先 shut down 队列再释放列表）；队列已停时 asyncRunTask 是空操作。
void MusicPlayList::postScanResultsReady(const std::weak_ptr<MusicPlayList> &weakSelf,
                                         const std::shared_ptr<ScanRequest> &request)
{
    std::shared_ptr<MusicPlayList> self = weakSelf.lock();
    if (self == nullptr || self->m_workQueue == nullptr) {
        return;
    }
    self->m_workQueue->asyncRunTask([weakSelf, request]() {
        std::shared_ptr<MusicPlayList> alive = weakSelf.lock();
        if (alive == nullptr) {
            return;
        }
        alive->onScanResultsReady(request);
    });
}

// 控制线程：按到达序取出请求里的就绪结果并插入（ADR 0021 决策 3/5/6）。
void MusicPlayList::onScanResultsReady(const std::shared_ptr<ScanRequest> &request)
{
    for (;;) {
        ScanProbeOutcome outcome;
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            if (request->ready.empty()) {
                break;
            }
            outcome = std::move(request->ready.front());
            request->ready.pop_front();
        }
        request->space.notify_one();
        request->pending.fetch_sub(1, std::memory_order_release);
        // 代际 + 取消双守卫（决策 6）：不是当前请求 / 已取消 ⇒ 丢弃，只递减计数，
        // 不产生任何回调（含汇总）。
        if (request->cancelled.load(std::memory_order_acquire) || !isCurrentScan(request)) {
            continue;
        }
        insertProbedTrack(outcome, false, request);
    }
    maybeFinishScan(request);
}

// 控制线程：结果全部落地后发一次汇总 + 列表载荷（ADR 0021 决策 6）。
void MusicPlayList::maybeFinishScan(const std::shared_ptr<ScanRequest> &request)
{
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        finished = request->scanFinished;
    }
    if (!finished || request->pending.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (request->cancelled.load(std::memory_order_acquire) || !isCurrentScan(request)) {
        return;
    }
    // 汇总只发一次：drain 与 finish 两个入口都可能看到「遍历结束 + 计数归零」。
    // 只在控制线程执行，普通 bool 足够。
    if (request->summaryReported) {
        return;
    }
    request->summaryReported = true;
    // 被取消的请求走不到这里（上面已返回）：取消是终态命令，零回调。
    reportScanSummaryFor(request);
    _updateList();
}

void MusicPlayList::_addTrack(const std::string &musicPath)
{
    addTrackSync(musicPath);
}

bool MusicPlayList::addTrackSync(const std::string &musicPath)
{
    // 传目录即丢弃（契约 §4.1）：目标只是「单个文件」。目录判定在队列线程做，
    // 调用者线程零文件系统访问（doc/buffering-switching-spec.md §5.1 的所有权清单）。
    if (pathIsDirectory(musicPath)) {
        LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, currentTraceId(),
                           "addTrack dropped: path is a directory (%s)", musicPath.c_str());
        return false;
    }
    const bool wasEmpty      = m_musicListProperties.empty();
    MusicPropertiesPtr added = addMusicInternal(musicPath, true);
    if (added == nullptr) {
        return false;
    }
    // 空列表首发时契约要求 PL 先于 TC（R1），所以「设为当前」必须排在发布之后。
    publishPlaylist();
    if (wasEmpty && m_selectMusicProperties == nullptr) {
        m_selectMusicProperties = added;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    }
    return true;
}

void MusicPlayList::_removeTrack(int index)
{
    removeTrackSync(index);
}

PlaylistEditOutcome MusicPlayList::removeTrackSync(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_musicListProperties.size()) {
        LogPrintfWithTrace(SdkLogLevel::Warning, LOG_TAG, currentTraceId(),
                           "removeTrack dropped: index=%d out of range (size=%zu)", index,
                           m_musicListProperties.size());
        return PlaylistEditOutcome::Dropped;
    }
    const size_t position = static_cast<size_t>(index);
    MusicPropertiesPtr removed = m_musicListProperties[position];
    const bool isCurrent       = (removed == m_selectMusicProperties);

    // pending 目标被删：作废在途切换（ADR 0020 决策 3——commit 守卫的 uid 校验
    // 是第二道防线）。若作废的是补位切换，「列表非空必有当前曲目」的不变量由
    // 本次删除的补位序重新建立（见下面的 !isCurrent 分支）。
    bool pendingSeatCancelled = false;
    if (m_pendingSwitch != nullptr && m_pendingSwitch->uid == removed->uid) {
        pendingSeatCancelled = (m_pendingSwitch->origin == SwitchOrigin::SeatReplacement);
        cancelPendingSwitch();
    }

    m_musicListProperties.erase(m_musicListProperties.begin() + position);
    // compaction（ADR 0018 §1）：删除即压缩，不留空洞；index 恒等于位置。
    for (size_t k = position; k < m_musicListProperties.size(); ++k) {
        m_musicListProperties[k]->index = k;
    }
    m_count.store(m_musicListProperties.size());

    // R1：载荷先发，随后同一命令引发的 TC 才有可解释的下标。
    publishPlaylist();

    if (!isCurrent) {
        if (pendingSeatCancelled && !m_musicListProperties.empty()
            && (m_selectMusicProperties == nullptr
                || findTrackByUid(m_selectMusicProperties->uid) == nullptr)) {
            // 被作废的补位 pending 意味着旧当前曲目已删、新落点未落地：重新补位
            //（可能同步 commit，也可能又转 pending），与 isCurrent 分支同构。
            if (selectFirstPlayable(seatingOrder(position), SwitchOrigin::SeatReplacement)) {
                return PlaylistEditOutcome::CurrentReplaced;
            }
            m_selectMusicProperties.reset();
            return PlaylistEditOutcome::CurrentCleared;
        }
        return PlaylistEditOutcome::RemovedOther;
    }
    if (m_musicListProperties.empty()) {
        m_selectMusicProperties.reset();
        return PlaylistEditOutcome::CurrentCleared;
    }
    // 补位不是 next（ADR 0018 §3）：只要列表非空就必须有当前曲目。
    if (!selectFirstPlayable(seatingOrder(position), SwitchOrigin::SeatReplacement)) {
        m_selectMusicProperties.reset();
        return PlaylistEditOutcome::CurrentCleared;
    }
    return PlaylistEditOutcome::CurrentReplaced;
}

void MusicPlayList::_clearPlaylist()
{
    clearPlaylistSync();
}

bool MusicPlayList::clearPlaylistSync()
{
    // 清空是终态命令：在途扫描一并作废（ADR 0021 决策 6，与 ADR 0020 决策 3 同形）。
    // 不取消就会在清空之后继续插入——旧路径里探测全部排在清空之前，语义是「清空胜」。
    cancelScan();
    // 清空是终态命令：在途切换作废（ADR 0020 决策 3）。不经播放器的直接调用
    //（测试）也要保证门闩被清、晚到结果被代际挡住。
    cancelPendingSwitch();
    if (m_musicListProperties.empty()) {
        // 幂等：零回调、零查询值变化（契约 §4.1）。
        return false;
    }
    m_musicListProperties.clear();
    m_count.store(0);
    m_selectMusicProperties.reset();
    // 列表已空，失败集合无意义；uid 不复用，留着只会无界增长（#263 §5）。
    clearFailedTracks();
    publishPlaylist();
    return true;
}

bool MusicPlayList::selectTrack(size_t index)
{
    if (m_count.load() == 0 || index >= m_musicListProperties.size()) {
        return false;
    }

    MusicPropertiesPtr next = m_musicListProperties.at(index);
    // 切歌窗口：旧任务只置取消位并移入退役槽（不 join，旧 ring 保活到回收），
    // 新任务用新的 ring，所以上一首的 PCM 不可能流进下一首（规格 §5.1 / 4.1 切歌行）。
    // ADR 0020：重开可能超窗转 pending——此时「意图已接受」但尚未 commit，
    // 选中项与当前任务都保持旧值，发布留给 onSwitchOpenCompleted。
    switch (beginSwitch(next, SwitchOrigin::User)) {
    case SwitchStart::Failed:
        return false;
    case SwitchStart::Pending:
        return true;
    case SwitchStart::Committed:
        publishSelection(next);
        return true;
    }
    return false;
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

std::weak_ptr<PlaybackTask> MusicPlayList::retiredTaskForTest() const
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    return std::weak_ptr<PlaybackTask>(m_retiredTask);
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
        if (m_currentTask != nullptr && m_currentTask->trackUid == current->uid
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
    // stop 是终态命令：在途切换一并作废（ADR 0020 决策 3）——否则晚到的 IO 结果
    // 会在设备已关之后重建任务。通知播放器清门闩。
    cancelPendingSwitch();
    // stop 是终态：先回收退役任务，再回收当前任务，两个都同步 join。
    joinRetiredTask();
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

void MusicPlayList::retireCurrentTask()
{
    PlaybackTaskPtr retired;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        retired.swap(m_currentTask);
    }
    if (retired == nullptr) {
        return;
    }
    // 只置取消位，不 join：join 会顶住消费节拍（规格 §5.1）。
    if (retired->decoder != nullptr) {
        retired->decoder->requestStop();
    }
    // 单槽：先回收上一个退役任务（其线程通常已退出，join 立即返回）。
    joinRetiredTask();
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_retiredTask = retired;
    }
}

void MusicPlayList::joinRetiredTask()
{
    PlaybackTaskPtr retired;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        retired.swap(m_retiredTask);
    }
    if (retired == nullptr) {
        return;
    }
    if (retired->decoder != nullptr) {
        retired->decoder->stop();
    }
    retired->decoder.reset();
    retired->ring.reset();
}

void MusicPlayList::reapRetiredTaskIfFinished()
{
    PlaybackTaskPtr retired;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        if (m_retiredTask == nullptr || m_retiredTask->decoder == nullptr
            || !m_retiredTask->decoder->isThreadFinished()) {
            return;
        }
        retired.swap(m_retiredTask);
    }
    // 线程已退出，stop() 里的 join 立即返回；仍由 WorkQueue 线程完成。
    retired->decoder->stop();
    retired->decoder.reset();
    retired->ring.reset();
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

    // play-from-stop 的重开（首播、暂停后任务已死的恢复）仍在本线程同步执行：
    // 这是调用方自己的命令在等自己的 open，不在 ADR 0020 的「切歌重开」范围内
    // （切歌路径一律走 beginSwitch，见决策 1；慢盘上首播等待属于既有行为）。
    // 重开在退役之前（#276 / 规格 player-state-contract.md §4.1「播放期重开失败」）：
    // acquire() 失败时旧任务未被退役，当前曲目继续播放、设备队列不清空。
    SourceLease lease(processProperties.source);
    if (!lease.valid()) {
        reportPlaybackOpenFailure(musicProperties, lease.status());
        return false;
    }
    std::shared_ptr<SourceLease> sharedLease = std::make_shared<SourceLease>(std::move(lease));
    return installSwitchTask(musicProperties, sharedLease, currentTraceId());
}

// 「租约已到手」到任务接管的公共尾部（ADR 0020 决策 2）：同步 commit 与 pending
// commit 两条出口共用，防止漂移。lease 必须已 valid；失败上报归调用方。
bool MusicPlayList::installSwitchTask(const MusicPropertiesPtr &musicProperties,
                                      std::shared_ptr<SourceLease> &lease,
                                      const std::string &traceId)
{
    // 切换收尾开销：旧任务退役（只置取消位，不 join）到新任务 start() 完成的那段时间。
    // pending 路径上不含等待 open 的部分——等待期已由 switch_open_ms 单独记录（#290）。
    const auto switchStart = std::chrono::steady_clock::now();

    // 先回收上一个退役任务（#277 判据 D3：窗口峰值保持在 2），再退役旧任务。
    // pending 路径上这一次 join 在回投时执行：等待期旧任务与在途 open 并存不占
    // 额外句柄（#286：卡住的 open 不持有文件句柄）。
    joinRetiredTask();

    // 切歌软取消（规格 §5）：旧任务只 abort 并移入退役槽，不 join；
    // 新任务立即启动，join 交给退役线程退出后的消费拍，或下一次切歌 / stop / 析构。
    retireCurrentTask();

    std::shared_ptr<AudioRingBuffer> ring
        = std::make_shared<AudioRingBuffer>(AudioRingBuffer::kDefaultCapacity);
    std::shared_ptr<AudioStreamDecoder> decoder
        = std::make_shared<AudioStreamDecoder>(ring.get(), m_devSpec);
    // start() 内部先 stop/join 自己的旧线程（本实例刚建，无旧线程），再起一个新解码线程。
    // ring 的 reset() 在新解码线程的第一件事里做（规格 4.4：只有解码线程能 reset）。
    sdk_utils::status_t ret = decoder->start(musicProperties->processProperties.extractor.get());
    if (ret != OK || decoder->state() == StreamDecoderState::ERROR) {
        decoder->stop();
        // 租约不移动：随调用方的 state / 局部量析构 release（失败路径不占句柄）。
        return false;
    }

    PlaybackTaskPtr task = std::make_shared<PlaybackTask>(ring, decoder, musicProperties->uid,
                                                          musicProperties, std::move(*lease));
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_currentTask = task;
    }

    const auto switchEnd = std::chrono::steady_clock::now();
    const auto switchMs  = std::chrono::duration_cast<std::chrono::milliseconds>(
                              switchEnd - switchStart)
                              .count();
    Metrics::RecordTiming("track_switch_ms", static_cast<uint64_t>(switchMs),
                          MetricTags { traceId, musicProperties->fileProperties.fullPath,
                                       static_cast<int>(musicProperties->index) });
    // 切歌完成边界的句柄预算快照（observability-contract.md §5.1，L2 只记录）。
    recordHandleGauges(MetricTags { traceId, musicProperties->fileProperties.fullPath,
                                    static_cast<int>(musicProperties->index) });
    return true;
}

// 提交一次切歌重开请求并做有界等待（ADR 0020 决策 1）。
MusicPlayList::SwitchStart MusicPlayList::beginSwitch(const MusicPropertiesPtr &musicProperties,
                                                      SwitchOrigin origin)
{
    if (musicProperties == nullptr) {
        return SwitchStart::Failed;
    }
    ProcessProperties &processProperties = musicProperties->processProperties;
    if (processProperties.extractor == nullptr) {
        reportError(ErrorCode::ExtractorInitFailed, "extractor is nullptr", musicProperties);
        return SwitchStart::Failed;
    }

    // 新意图顶替旧 pending：旧状态立即失效（代际在下面递增），晚到的旧结果会在
    // 守卫处被丢弃；旧租约随最后一个引用释放。在途请求无法取消（#286）——执行器
    // 串行，新请求要等旧系统调用返回才开始（ADR 0020「负向代价」已登记）。
    m_pendingSwitch.reset();
    ++m_switchGeneration;

    // 先回收上一个退役任务：acquire 成功后新旧租约短暂并存，峰值保持在 2（#277 D3）。
    joinRetiredTask();

    std::shared_ptr<PendingSwitch> state = std::make_shared<PendingSwitch>();
    state->generation = m_switchGeneration;
    state->uid        = musicProperties->uid;
    state->properties = musicProperties;
    state->origin     = origin;
    state->begin      = std::chrono::steady_clock::now();
    // 指标标签在提交时抄下来：等待期 traceId 可能已被后续命令刷新（ADR 0020）。
    state->traceId    = currentTraceId();
    state->path       = musicProperties->fileProperties.fullPath;
    state->index      = static_cast<int>(musicProperties->index);

    // 有界等待：open 在 IO 执行器线程上执行，控制线程至多等 kSwitchOpenWaitMs。
    // job 只写 lease / status；回投与 commit 都在控制队列上做。捕获一律用弱引用：
    // 目标析构后 job 不得再碰 MusicPlayList 的任何成员（决策 5）。
    std::weak_ptr<PendingSwitch> weakState = state;
    // shared_ptr 临时量立刻转存为弱引用：弱引用只延长控制块引用，不延长寿命。
    std::weak_ptr<MusicPlayList> weakSelf = shared_from_this();
    SwitchOpenExecutor::WaitResult wait    = m_openExecutor->runBounded(
        std::chrono::milliseconds(kSwitchOpenWaitMs), [weakState, weakSelf]() {
            std::shared_ptr<PendingSwitch> s = weakState.lock();
            if (s == nullptr) {
                return; // 状态已作废：不打开、不回投，栈引用释放时租约为空
            }
            const auto openStart = std::chrono::steady_clock::now();
            SourceLease lease(s->properties->processProperties.source);
            const auto openMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - openStart)
                                    .count();
            // 切歌 open 段单独计时（#290，L2 只记录不设阈值）：等待窗口与收尾拆开看。
            Metrics::RecordTiming("switch_open_ms", static_cast<uint64_t>(openMs),
                                  MetricTags { s->traceId, s->path, s->index });
            s->lease  = std::make_shared<SourceLease>(std::move(lease));
            s->status = s->lease->status();
            // 结果回投控制队列。lock 失败 = MusicPlayList 已在析构：结果丢弃，
            // 租约随 s 的栈引用在函数尾释放（决策 5：不碰已析构对象）。
            std::shared_ptr<MusicPlayList> self = weakSelf.lock();
            if (self == nullptr) {
                return;
            }
            self->postSwitchOpenCompleted(weakState);
        });

    if (wait == SwitchOpenExecutor::WaitResult::Stopped) {
        // 执行器已停止（只发生在析构竞态）：按重开失败处理，旧任务未动。
        reportPlaybackOpenFailure(musicProperties, sdk_utils::UNKNOWN_ERROR);
        return SwitchStart::Failed;
    }
    if (wait == SwitchOpenExecutor::WaitResult::Completed) {
        // 50 ms 内拿到结果：同步 commit（SSD 常态路径，行为与既有切歌一致）。
        if (state->lease == nullptr || !state->lease->valid()) {
            reportPlaybackOpenFailure(musicProperties,
                                      state->lease != nullptr ? state->lease->status()
                                                              : sdk_utils::UNKNOWN_ERROR);
            return SwitchStart::Failed;
        }
        if (commitSwitch(state)) {
            return SwitchStart::Committed;
        }
        // commitSwitch 内部失败（decoder start 等）：与同步路径一致不上报，记账归调用方。
        return SwitchStart::Failed;
    }

    // 超窗（ADR 0020 决策 1/2）：意图已接受，控制线程立即交还；commit 由 IO 结果
    // 回投触发。等待期旧任务继续出声、门闩由播放器保持；作废走 cancelPendingSwitch。
    Metrics::RecordCount("switch_open_pending_total", 1,
                         MetricTags { state->traceId, state->path, state->index });
    m_pendingSwitch = state;
    m_switchPending.store(true);
    return SwitchStart::Pending;
}

// 把 IO 结果投回控制队列（IO 执行器线程调用）。
void MusicPlayList::postSwitchOpenCompleted(const std::weak_ptr<PendingSwitch> &state)
{
    if (m_workQueue == nullptr) {
        return;
    }
    std::weak_ptr<MusicPlayList> weakSelf = shared_from_this();
    m_workQueue->asyncRunTask([weakSelf, state]() {
        // 双弱引用判活：MusicPlayList 已析构或状态已作废都静默丢弃（ADR 0020 决策 5）。
        std::shared_ptr<MusicPlayList> self = weakSelf.lock();
        std::shared_ptr<PendingSwitch> s    = state.lock();
        if (self == nullptr || s == nullptr) {
            return;
        }
        self->onSwitchOpenCompleted(s);
    });
}

// pending 切换的 commit 出口：双守卫后走公共收尾（ADR 0020 决策 2/3）。
bool MusicPlayList::commitSwitch(const std::shared_ptr<PendingSwitch> &state)
{
    // 代际仍是当前值。
    if (state->generation != m_switchGeneration) {
        return false;
    }
    // 目标条目仍在列表中、仍是同一条目（指针相等；uid 不复用，指针校验是第二道防线）。
    MusicPropertiesPtr stillThere = findTrackByUid(state->uid);
    if (stillThere == nullptr || stillThere != state->properties) {
        return false;
    }
    return installSwitchTask(state->properties, state->lease, state->traceId);
}

// IO 结果回投的处理（控制队列线程）：守卫、提交或失败结算。
void MusicPlayList::onSwitchOpenCompleted(const std::shared_ptr<PendingSwitch> &state)
{
    // 双守卫（ADR 0020 决策 3）：只认「仍是当前 pending、代际未变」的结果。
    // 过期结果静默丢弃；租约随 state 的最后一个引用析构 release。
    if (m_pendingSwitch != state || state->generation != m_switchGeneration) {
        return;
    }
    m_pendingSwitch.reset();
    m_switchPending.store(false);

    const bool openOk = state->lease != nullptr && state->lease->valid();
    if (openOk && commitSwitch(state)) {
        // 发布选中项（TC/DC/PC）：与同步路径同一次序——发布先于 settlement 通知。
        publishSelection(state->properties);
        if (m_callback != nullptr) {
            // 延后的净空 / 起播由播放器在 settlement 里补齐（决策 2/3）。
            m_callback->onPendingSwitchSettled(SwitchSettlement::Committed, state->origin);
        }
        return;
    }
    if (openOk) {
        // open 成功但 commit 内部失败（decoder start 等）：与同步路径一致不上报。
        settlePendingFailure(state);
        return;
    }
    // open 失败：按 #276 语义上报（一次 onError、记账），再续做 / 终态。
    reportPlaybackOpenFailure(state->properties,
                              state->lease != nullptr ? state->lease->status()
                                                      : sdk_utils::UNKNOWN_ERROR);
    settlePendingFailure(state);
}

// pending 失败后的候选续做与终态通知（ADR 0020 决策 4）。
void MusicPlayList::settlePendingFailure(const std::shared_ptr<PendingSwitch> &state)
{
    // 自动跳轨 / 补位从下一个候选继续；中间候选不产生可见的切换事件，一次命令
    // 最多一个最终结果。注意：等待期发生过的 compaction 会让候选下标漂移，
    // tryCandidates 的越界防御与 commit 的 uid 守卫兜底（最坏是跳错一首，不崩溃）。
    if (state->nextCandidate < state->order.size()
        && tryCandidates(state->order, state->nextCandidate, state->origin)) {
        return; // 新候选已接受（同步 commit 或又转 pending）
    }
    if (state->origin == SwitchOrigin::SeatReplacement) {
        // 与同步 CurrentCleared 一致：补位耗尽，当前曲目置空；播放器落 Stopped。
        m_selectMusicProperties.reset();
    }
    if (m_callback != nullptr) {
        m_callback->onPendingSwitchSettled(SwitchSettlement::Failed, state->origin);
    }
}

// 发布「已切到这条」：位置归零、重置其他条目进度、回调 TC（同步与 pending 共用）。
void MusicPlayList::publishSelection(const MusicPropertiesPtr &musicProperties)
{
    resetTrackProgressExcept(musicProperties->uid);
    musicProperties->signalProperties.curPositionMs = 0;
    m_selectMusicProperties                         = musicProperties;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}

// 作废 pending 并通知播放器清门闩（ADR 0020 决策 3）。notify=false 用于析构：
// 回调对象可能已半析构，只作废、只递增代际，不通知。
void MusicPlayList::cancelPendingSwitch(bool notify)
{
    if (m_pendingSwitch == nullptr) {
        return;
    }
    const SwitchOrigin origin = m_pendingSwitch->origin;
    // 代际递增：晚到的 IO 结果在 onSwitchOpenCompleted 守卫处被静默丢弃。
    ++m_switchGeneration;
    m_pendingSwitch.reset();
    m_switchPending.store(false);
    if (notify && m_callback != nullptr) {
        m_callback->onPendingSwitchSettled(SwitchSettlement::Cancelled, origin);
    }
}

MusicPropertiesPtr MusicPlayList::findTrackByUid(uint64_t uid) const
{
    for (const MusicPropertiesPtr &entry : m_musicListProperties) {
        if (entry != nullptr && entry->uid == uid) {
            return entry;
        }
    }
    return nullptr;
}

bool MusicPlayList::switchPending() const
{
    return m_switchPending.load();
}

void MusicPlayList::resetTrackProgressExcept(uint64_t keepUid)
{
    for (auto &item : m_musicListProperties) {
        if (item == nullptr || item->uid == keepUid) {
            continue;
        }
        item->signalProperties.curPositionMs = 0;
        item->signalProperties.dataSize      = 0;
    }
}

void MusicPlayList::reportPlaybackOpenFailure(const MusicPropertiesPtr &musicProperties,
                                              sdk_utils::status_t status)
{
    if (musicProperties == nullptr) {
        return;
    }
    const char *detail = openFailureDetailForStatus(status);
    // 一次失败即记账：本次播放会话内不再重试，补位序与 skipToNextPlayable 自然跳过。
    markTrackFailed(musicProperties->uid);
    LogPrintfWithTrace(SdkLogLevel::Error, LOG_TAG, currentTraceId(),
                       "error=%d detail=%s index=%d path=%s",
                       static_cast<int>(ErrorCode::FileOpenFailed), detail,
                       static_cast<int>(musicProperties->index),
                       musicProperties->fileProperties.fullPath.c_str());
    // 播放期失败必须可见：绕过 reportError 的批量扫描降噪门槛（用户正在等这次切歌）。
    // 走独立回调而不是 onMusicPlayListError：播放器层对入列失败的处置是「停设备 + 置
    // Stopped」，而本失败要求不改播放状态（旧任务仍在播）。
    if (m_callback != nullptr) {
        m_callback->onPlaybackOpenFailed(ErrorCode::FileOpenFailed, detail,
                                         static_cast<int>(musicProperties->index),
                                         musicProperties->fileProperties.fullPath);
    }
}

void MusicPlayList::reportError(ErrorCode code, const std::string &detail,
                                const MusicPropertiesPtr &musicProperties, bool userInitiated)
{
    int index = -1;
    std::string path;
    if (musicProperties != nullptr) {
        // 用户显式命令失败时该条目从未入列，没有位置可言（契约 §4.1：index 固定 -1）。
        if (!userInitiated) {
            index = static_cast<int>(musicProperties->index);
        }
        path = musicProperties->fileProperties.fullPath;
    }
    std::string traceId = currentTraceId();
    LogPrintfWithTrace(SdkLogLevel::Error, LOG_TAG, traceId, "error=%d detail=%s index=%d path=%s",
                       static_cast<int>(code), detail.c_str(), index, path.c_str());
    // 批量扫描的降噪门槛（已有当前曲目时不逐文件报错）不适用于用户显式命令：
    // 那条门槛会让一次 `addTrack` 的失败完全不可见（契约 §4.1「失败三类」）。
    bool shouldNotify = userInitiated || m_selectMusicProperties == nullptr
                        || musicProperties == m_selectMusicProperties;
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
