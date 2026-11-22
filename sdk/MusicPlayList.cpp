#include "MusicPlayList.h"
#include "ExtractorFactory.h"
#include "HttpSource.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "Metrics.h"
#include "UrlSource.h"
#include "Utf8Path.h"
#include "type_name.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cwctype>
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
    return selectFirstPlayable(order);
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

bool MusicPlayList::selectFirstPlayable(const std::vector<size_t> &order)
{
    const size_t count = m_musicListProperties.size();
    for (size_t index : order) {
        if (index >= count) {
            continue; // 防御：候选序由 seatingOrder 生成，正常情况下必然在范围内
        }
        MusicPropertiesPtr candidate = m_musicListProperties.at(index);
        if (isTrackFailed(candidate->uid)) {
            continue;
        }
        if (!replacePlaybackTask(candidate)) {
            markTrackFailed(candidate->uid);
            continue;
        }
        resetTrackProgressExcept(candidate->uid);
        candidate->signalProperties.curPositionMs = 0;
        m_selectMusicProperties                   = candidate;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
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

void MusicPlayList::recordScanRejection(ErrorCode code)
{
    std::lock_guard<std::mutex> lock(m_rejectMutex);
    m_scanRejects[code] += 1;
}

void MusicPlayList::reportScanSummary()
{
    std::map<ErrorCode, int> rejects;
    std::map<std::string, int> causes;
    {
        std::lock_guard<std::mutex> lock(m_rejectMutex);
        rejects.swap(m_scanRejects);
        causes.swap(m_scanRejectCauses);
    }
    // 句柄预算（observability-contract.md §5.1，L2 只记录不设阈值）：
    // 挂在这个边界上，千曲扫描不会产生 O(n) 条仪表日志。
    {
        sdk::MetricTags tags(currentTraceId(), std::string(), -1);
        sdk::Metrics::RecordGauge("datasource_open_handles",
                                  static_cast<double>(FileSource::openHandleCountForTest()), tags,
                                  "count");
        sdk::Metrics::RecordGauge("datasource_open_peak",
                                  static_cast<double>(FileSource::peakOpenHandleCountForTest()),
                                  tags, "count");
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

void MusicPlayList::recordScanRejectCause(const char *cause)
{
    if (cause == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_rejectMutex);
    m_scanRejectCauses[cause] += 1;
}
void MusicPlayList::_addMusic(const std::string &musicPath)
{
    addMusicInternal(musicPath, false);
}

MusicPropertiesPtr MusicPlayList::addMusicInternal(const std::string &musicPath, bool userInitiated)
{
    auto openStart                       = std::chrono::steady_clock::now();
    MusicPropertiesPtr musicProperties   = std::make_shared<MusicProperties>();
    musicProperties->index               = m_musicListProperties.size();
    FileProperties &fileProperties       = musicProperties->fileProperties;
    SignalProperties &signalProperties   = musicProperties->signalProperties;
    ProcessProperties &processProperties = musicProperties->processProperties;
    fileProperties.fullPath              = musicPath;
    // 身份键的唯一产生点（ADR 0014）：addMusic / addMusicWithNotify / URL 直通三条入口
    // 都经过 _addMusic，归一化只算这一次，下游（MusicIndex → TrackInfo::id）原样传递。
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
                // 用户显式命令的失败不走批次汇总：一次 onError 已足够，也不留拒绝累计
                // 去串进下一次目录扫描（契约 §4.1「失败三类」）。
                if (!userInitiated) {
                    recordScanRejection(ErrorCode::FileOpenFailed);
                }
                reportError(ErrorCode::FileOpenFailed,
                            "playlist content has neither VOD nor LIVE features",
                            musicProperties, userInitiated);
                return nullptr;
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
                    if (!userInitiated) {
                        recordScanRejection(ErrorCode::FileOpenFailed);
                    }
                    reportError(ErrorCode::FileOpenFailed,
                                "icy source native url open failed",
                                musicProperties, userInitiated);
                    return nullptr;
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
        if (!userInitiated) {
            recordScanRejection(ErrorCode::FileOpenFailed);
        }
        reportError(ErrorCode::FileOpenFailed, "new DataSource failed", musicProperties,
                    userInitiated);
        return nullptr;
    }
    if (source->initCheck() != OK) {
        // 成因分级（#278）：错误码仍是 FileOpenFailed，detail 区分句柄耗尽 / 权限 / 找不到。
        const status_t openStatus = source->openStatus();
        const char *detail        = openFailureDetailForStatus(openStatus);
        if (!userInitiated) {
            recordScanRejection(ErrorCode::FileOpenFailed);
            recordScanRejectCause(detail);
        }
        reportError(ErrorCode::FileOpenFailed, detail, musicProperties, userInitiated);
        return nullptr;
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
        if (!userInitiated) {
            recordScanRejection(code);
        }
        reportError(code, detail, musicProperties, userInitiated);
        return nullptr;
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
    // 探测结束即释放构造租约（streaming-playback-spec.md §4.5）：条目只留数据源对象、
    // extractor 与已解析元数据，常驻句柄数不再随条目数增长。播放前由 PlaybackTask 重开。
    processProperties.source->release();
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
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

    m_musicListProperties.erase(m_musicListProperties.begin() + position);
    // compaction（ADR 0018 §1）：删除即压缩，不留空洞；index 恒等于位置。
    for (size_t k = position; k < m_musicListProperties.size(); ++k) {
        m_musicListProperties[k]->index = k;
    }
    m_count.store(m_musicListProperties.size());

    // R1：载荷先发，随后同一命令引发的 TC 才有可解释的下标。
    publishPlaylist();

    if (!isCurrent) {
        return PlaylistEditOutcome::RemovedOther;
    }
    if (m_musicListProperties.empty()) {
        m_selectMusicProperties.reset();
        return PlaylistEditOutcome::CurrentCleared;
    }
    // 补位不是 next（ADR 0018 §3）：只要列表非空就必须有当前曲目。
    if (!selectFirstPlayable(seatingOrder(position))) {
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
    if (!replacePlaybackTask(next)) {
        return false;
    }
    resetTrackProgressExcept(next->uid);
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

    // 切歌开销：旧任务退役（只置取消位，不 join）到新任务 start() 完成的那段时间。
    // 与 init 只读扫描耗时同构——只记录，不进首播判据、不设阈值（规格 4.3 / 11）。
    const auto switchStart = std::chrono::steady_clock::now();

    // 先回收上一个退役任务，再重开：否则「当前 + 上一个退役 + 新任务」会让峰值瞬时到 3。
    // joinRetiredTask 本来就由 retireCurrentTask 在同一个逻辑点调用，提前到重开之前不改变
    // 任何时序语义；代价与既有切歌相同（旧线程已退出时 join 立即返回）。
    joinRetiredTask();

    // 重开在退役之前（#276 / 规格 player-state-contract.md §4.1「播放期重开失败」）：
    // acquire() 失败时旧任务未被退役，当前曲目继续播放、设备队列不清空——
    // 否则会留下「解码器已停、状态仍是 Playing」的静音窗口。
    SourceLease lease(processProperties.source);
    if (!lease.valid()) {
        reportPlaybackOpenFailure(musicProperties, lease.status());
        return false;
    }

    // 切歌软取消（规格 §5）：旧任务只 abort 并移入退役槽，不 join；
    // 新任务立即启动，join 交给退役线程退出后的消费拍，或下一次切歌 / stop / 析构。
    retireCurrentTask();

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

    PlaybackTaskPtr task = std::make_shared<PlaybackTask>(ring, decoder, musicProperties->uid,
                                                          musicProperties, std::move(lease));
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
    // 切歌完成边界的句柄预算快照（observability-contract.md §5.1，L2 只记录）。
    Metrics::RecordGauge("datasource_open_handles",
                         static_cast<double>(FileSource::openHandleCountForTest()),
                         MetricTags { currentTraceId(), musicProperties->fileProperties.fullPath,
                                      static_cast<int>(musicProperties->index) },
                         "count");
    Metrics::RecordGauge("datasource_open_peak",
                         static_cast<double>(FileSource::peakOpenHandleCountForTest()),
                         MetricTags { currentTraceId(), musicProperties->fileProperties.fullPath,
                                      static_cast<int>(musicProperties->index) },
                         "count");
    return true;
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
    if (m_callback != nullptr) {
        m_callback->onMusicPlayListError(ErrorCode::FileOpenFailed, detail,
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
