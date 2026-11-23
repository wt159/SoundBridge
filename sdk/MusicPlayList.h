#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioRingBuffer.h"
#include "AudioStreamDecoder.h"
#include "ErrorCode.hpp"
#include "ExtractorHelper.hpp"
#include "FileSource.h"
#include "NonCopyable.hpp"
#include "PlaybackMode.h"
#include "SwitchOpenExecutor.h"
#include "WorkQueue.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sdk {

// 派生曲目身份键（ADR 0014）：绝对化 → 词法归一（消 `.` 与 `..`）→ 分隔符统一 `/`
// → Windows 下按宽字符折叠大小写。`http(s)://` 原样返回、不做归一化。
// 只做词法处理，不碰文件系统（不解析符号链接、不要求路径存在），因此文件删除后
// 同一路径的键不变。**不保证列表内唯一**：`addMusic` 无去重。
std::string deriveMusicId(const std::string &path);

struct FileProperties {
    std::string fullPath;
    // fullPath 的归一化形式，入列时由 deriveMusicId() 计算（唯一 choke point）。
    std::string musicId;
    std::string fileName;
    std::string extensionName;
    std::string fileDir;

    void parseFileName()
    {
        if (fullPath.empty())
            return;
        // Find the last slash position.
        size_t lastSlashPos = fullPath.find_last_of("/");
        if (lastSlashPos != std::string::npos) {
            // Extract the directory path.
            fileDir           = fullPath.substr(0, lastSlashPos + 1);
            // Extract the file name.
            fileName          = fullPath.substr(lastSlashPos + 1);
            // 仅 URL 可能带 query / fragment：切最后一个点之前先去掉，避免扩展名被
            // "?token=..." / "#frag" 污染。本地 POSIX 文件名可合法包含 '#'，
            // 不能被误剥离，因此只在 http:// / https:// 前缀时才剥离。
            const bool isUrl = fullPath.compare(0, 7, "http://") == 0
                            || fullPath.compare(0, 8, "https://") == 0;
            if (isUrl) {
                const size_t queryOrFragment = fileName.find_first_of("?#");
                if (queryOrFragment != std::string::npos) {
                    fileName = fileName.substr(0, queryOrFragment);
                }
            }
            // Find the last dot position.
            size_t lastDotPos = fileName.find_last_of(".");
            if (lastDotPos != std::string::npos) {
                // Extract the extension.
                extensionName = fileName.substr(lastDotPos);
                // Remove the extension from the file name.
                fileName      = fileName.substr(0, lastDotPos);
            }
        }
    }
};

struct SignalProperties {
    AudioSpec spec;
    uint64_t durationMs;
    // 唯一位置口径：来自 AudioStreamDecoder::positionMs()（段基准 + ring 段内读计数）。
    std::atomic<uint64_t> curPositionMs;
    off64_t dataSize;
};

struct ProcessProperties {
    std::shared_ptr<DataSource> source;
    std::shared_ptr<ExtractorHelper> extractor;
};

struct MusicProperties {
    size_t index;
    // 进程内曲目身份（ADR 0018）：只在对象生命周期内唯一，不跨启动、不进公开面。
    // index 会因 compaction 而变化，但要判「是不是同一曲」的地方一律比 uid。
    // 0 永远不是有效 uid（从 1 起分配）。
    uint64_t uid;
    FileProperties fileProperties;
    SignalProperties signalProperties;
    ProcessProperties processProperties;
};

using MusicPropertiesPtr = std::shared_ptr<MusicProperties>;

// 一次播放任务：ring 与 decoder 由**当前曲目独占**，跨曲目不复用（规格 #55 第 4.3 节）。
//
// - 创建者：播放列表在做「这次要播哪首」的决定时造；消费者只借指针，不持有所有权。
// - 销毁时机：切歌时旧任务移入退役槽（abort 不 join），join 交给退役线程退出后的消费拍
//   （`reapRetiredTaskIfFinished()`）或下一次切歌 / stop / 析构（规格
//   doc/buffering-switching-spec.md §5）；旧 ring 保活到 join 完成。
// - 成员声明顺序决定析构顺序：decoder 声明在 ring 之后，所以先析构 decoder（其析构函数
//   join 掉自己的解码线程），再释放它只持有裸指针的 ring。`owner` 声明在最前，因此最后
//   析构——解码线程只持有 extractor 的裸指针，条目必须活到线程退出。
struct PlaybackTask {
    PlaybackTask(const std::shared_ptr<AudioRingBuffer> &ringBuffer,
                 const std::shared_ptr<AudioStreamDecoder> &streamDecoder, uint64_t trackIdentity,
                 const MusicPropertiesPtr &owningProperties, SourceLease &&sourceLease)
        : owner(owningProperties)
        , lease(std::move(sourceLease))
        , ring(ringBuffer)
        , decoder(streamDecoder)
        , trackUid(trackIdentity)
    {
    }

    // extractor 的保活引用（#268 实证的 UAF）：decoder 只拿 `extractor.get()` 的裸指针，
    // 以前靠「条目一直留在 m_musicListProperties 里」这个偶然事实续命。removeTrack 把这个
    // 前提打破了——条目一 erase，extractor 立刻析构，而退役任务只 abort 不 join，解码线程
    // 还在读它。MinGW 上释放的堆块仍映射所以没崩，glibc 直接 SIGSEGV。
    // 与 ring 同一策略：保活到 join 完成。
    MusicPropertiesPtr owner;
    // 数据源租约（规格 streaming-playback-spec.md §4.5）：声明在 ring / decoder 之前，
    // 析构逆序 ⇒ decoder 先 stop()（内含 join）、租约后 release()。「关闭晚于 join」
    // 由声明顺序结构性保证，三个收尾点不需要显式 close，也不需要同一性守卫。
    SourceLease lease;
    std::shared_ptr<AudioRingBuffer> ring;
    std::shared_ptr<AudioStreamDecoder> decoder;
    // 曲目身份（ADR 0018）：位置会因 compaction 变化，uid 不变。所有「这个任务是不是
    // 当前曲目的」判据都只认它，不再比较 index。
    uint64_t trackUid;
};

using PlaybackTaskPtr = std::shared_ptr<PlaybackTask>;

// 列表编辑的结果（ADR 0018 / 契约 §4.1）：播放器层据此决定「补位后是否恢复播放」「是否停机」。
// 列表层只做「删条目 + 重编号 + 发布载荷 + 选落点」，状态机仍归 MusicPlayer。
enum class PlaylistEditOutcome {
    Dropped,         // 非法下标 / 空列表：丢弃，零回调、零查询值变化（契约 §4.1）
    RemovedOther,    // 删的不是当前曲目；当前曲目的 index 可能已变（R2 由播放器层补 TC）
    CurrentReplaced, // 删的是当前曲目，已补位到新落点（当前曲目已换、已发 TC）
    CurrentCleared,  // 删的是当前曲目，列表为空或全部不可播：当前曲目已置空
};

// pending 切歌的最终结果（ADR 0020，地图 #285 / #287）。同步路径不会产生本事件——
// 调用方当场拿到返回值，按既有 endSegmentSwitch 收尾。
enum class SwitchSettlement {
    Committed, // 已 commit：TC / DC / PC 已按既有次序发出
    Failed,    // 重开失败：已按 #276 上报，状态不变
    Cancelled, // 被 stop / clearPlaylist / 删除目标曲目作废：零回调，只清门闩
};

// 切换意图的来源。失败终态不同：用户命令不改播放状态；自动跳过停在 Paused
//（解码错误）或 Stopped（设备错误）；段末（EOS）前进停到 Stopped；删除当前
// 曲目的补位耗尽等价 CurrentCleared → Stopped——与既有同步路径逐字一致。
enum class SwitchOrigin {
    User,
    AutoSkip,
    EndOfStream,
    SeatReplacement,
};

class MusicPlayListCallback {
public:
    virtual ~MusicPlayListCallback()                                    = default;
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property)    = 0;
    virtual void updateMusicList(std::vector<MusicPropertiesPtr> &list) = 0;
    virtual void onMusicPlayListError(ErrorCode code, const std::string &detail, int index,
                                      const std::string &path)
        = 0;
    // pending 切歌的最终结果（ADR 0020 决策 2/3/5）。默认空实现，既有回调无需改动。
    virtual void onPendingSwitchSettled(SwitchSettlement /*settlement*/,
                                        SwitchOrigin /*origin*/)
    {
    }
    // 播放期重开失败（规格 player-state-contract.md §4.1，地图 #273 / #276）：与入列失败分开，
    // 因为播放器层对后者的处置是「停设备 + 置 Stopped」，而重开失败要求**不改播放状态**。
    // 默认转发到 onMusicPlayListError，既有回调实现无需改动。
    virtual void onPlaybackOpenFailed(ErrorCode code, const std::string &detail, int index,
                                      const std::string &path)
    {
        onMusicPlayListError(code, detail, index, path);
    }
};

class MusicPlayList : public NonCopyable, public std::enable_shared_from_this<MusicPlayList> {
public:
    // 控制线程等待 open 的上限（ADR 0020 决策 1）：100 ms 控制预算的一半留给
    // 「退役旧任务 + 建 decoder + 清设备队列」，stop 恰逢等待起点时最坏 50 ms 生效。
    static const int kSwitchOpenWaitMs = 50;
private:
    MusicPlayListCallback *m_callback;
    // 条目数镜像（恒等于 m_musicListProperties.size()），只在队列线程写、任意线程读。
    // 原来的名字 m_curIndex 会诱导人把它当「当前下标」，它就是计数。
    std::atomic<size_t> m_count;
    WorkQueue *m_workQueue;
    AudioSpec m_devSpec;
    MusicPropertiesPtr m_selectMusicProperties;
    std::vector<MusicPropertiesPtr> m_musicListProperties;
    mutable std::mutex m_traceMutex;
    std::string m_traceId;

public:
    MusicPlayList() = delete;
    MusicPlayList(MusicPlayListCallback *callback, WorkQueue *wq, AudioSpec &devSpec);
    ~MusicPlayList();

    void addMusic(const std::string &musicPath);
    void addMusicWithNotify(const std::string &musicPath, std::function<void()> onDone);
    // 列表编辑（ADR 0018 / 契约 §4.1）：三个命令都是**纯入队**，调用者线程零文件系统访问。
    void addTrack(const std::string &musicPath);
    void removeTrack(int index);
    void clearPlaylist();
    // 同步版本只在 WorkQueue 线程上执行（测试可直接调用）。语义与异步版本一致。
    bool addTrackSync(const std::string &musicPath);
    PlaylistEditOutcome removeTrackSync(int index);
    bool clearPlaylistSync();
    void next();
    void pervious();
    void setCurrentIndex(int index);
    void updateList();
    int getMusicCount();
    void setPlaybackMode(MusicPlaybackMode mode);
    MusicPlaybackMode playbackMode() const;
    bool nextSync();
    bool previousSync();
    bool setCurrentIndexSync(int index);
    void setTraceId(const std::string &traceId);
    bool advanceToNextTrack();
    bool skipToNextPlayable();

    // 当前播放任务（ring + decoder + 曲目号）。没有任务时返回空：停止状态、切歌窗口、
    // 以及扫描阶段都属于这一类。调用方拿到的是快照，持有期间任务不会被销毁。
    PlaybackTaskPtr currentTask() const;
    // 是否处于「已接受切换意图、重开结果尚未回来」的等待期（ADR 0020）。等待期
    // 当前曲目与当前任务都还是旧的；commit / 失败 / 作废之后归 false。
    bool switchPending() const;
    // 仅供测试（ADR 0011）：退役槽的弱引用快照。不延长任务生命周期，任务销毁后自动过期；
    // 用于观察「切换窗口是否关闭」，不暴露可操作句柄。
    std::weak_ptr<PlaybackTask> retiredTaskForTest() const;
    // 为「当前曲目」准备播放任务。已经有一个活着的任务（暂停后恢复）就复用；
    // 没有 / 已结束 / 已失败则把旧任务移入退役槽（只置取消位，不 join），再建新 ring
    // 与新 decoder（规格 §5.1）。
    bool startCurrentTask();
    // 丢弃当前任务：先回收退役任务，再 join 当前解码线程，然后销毁 ring
    // （旧 PCM 一律丢弃、不 drain）。stop 是终态，两个槽都同步 join。
    void stopCurrentTask();
    // 非阻塞回收：退役任务的解码线程已退出时 join 并释放。消费定时任务每拍调用，
    // 让「切换窗口」在旧线程退出后立即结束，而不是拖到下一次控制事件（规格 §5.3）。
    void reapRetiredTaskIfFinished();

    // 本次播放会话内因坏数据失败过的曲目：自动跳过不再回头重试（同一曲不重试第二次）。
    // 用户按播放（MusicPlayer::play）时清空，视为新会话。
    void markTrackFailed(uint64_t uid);
    bool isTrackFailed(uint64_t uid) const;
    void clearFailedTracks();
    // 仅供测试（ADR 0011/0018）：第 index 条曲目的进程内 uid；越界返回 0。
    // 失败集合按 uid 记账，测试要用它把「第几条」翻译成身份。
    uint64_t trackUidForTest(size_t index) const;
    // 仅供测试：第 index 条条目的弱引用；越界返回空。
    // 用途是断言**所有权**而非「值」——删除当前曲目后，退役任务必须仍持有该条目，
    // 否则解码线程读的就是已释放的 extractor（#268 在 glibc 上的 SIGSEGV）。
    std::weak_ptr<MusicProperties> trackRefForTest(size_t index) const;

    // 批量扫描汇总：被拒文件不进播放列表，永远不会成为「当前曲目」，
    // 只能靠这份按错误码分桶的汇总把它们报给 UI（规格 5.4 H6）。
    void reportScanSummary();

protected:
    void _addMusic(const std::string &musicPath);
    void _addTrack(const std::string &musicPath);
    void _removeTrack(int index);
    void _clearPlaylist();
    void _next();
    void _pervious();
    void _setCurrentIndex(int index);
    void _updateList();

private:
    bool selectTrack(size_t index);
    // begin / commit 两阶段（ADR 0020 决策 2）。
    enum class SwitchStart {
        Failed,    // 当场失败：已按 #276 上报，旧任务未动
        Committed, // 50 ms 内完成并已提交，调用方按既有同步路径收尾
        Pending,   // 超窗：意图已接受，commit 由 IO 结果回投触发
    };
    struct PendingSwitch;
    // 提交一次重开请求并做有界等待。origin 决定 pending 失败后的终态与候选续做。
    SwitchStart beginSwitch(const MusicPropertiesPtr &musicProperties, SwitchOrigin origin);
    // 建 ring / decoder、退役旧任务、发布选中项。同步与 pending 两条出口共用。
    bool commitSwitch(const std::shared_ptr<PendingSwitch> &state);
    // IO 结果回投（控制队列）：代际 + uid 双守卫。
    void onSwitchOpenCompleted(const std::shared_ptr<PendingSwitch> &state);
    // 把 IO 结果投回控制队列（IO 执行器线程调用；目标析构则静默丢弃）。
    void postSwitchOpenCompleted(const std::weak_ptr<PendingSwitch> &state);
    // 「租约已到手」到任务接管的公共尾部（ADR 0020 决策 2）：同步 commit 与
    // pending commit 两条出口共用，防止漂移。lease 必须已 valid；失败上报归调用方。
    bool installSwitchTask(const MusicPropertiesPtr &musicProperties,
                           std::shared_ptr<SourceLease> &lease, const std::string &traceId);
    // 按候选序试播：跳过已失败条目，beginSwitch 失败即记账并继续；
    // 命中（Committed / Pending）返回 true。
    bool tryCandidates(const std::vector<size_t> &order, size_t from, SwitchOrigin origin);
    // pending 失败后的续做与终态通知（ADR 0020 决策 4）。
    void settlePendingFailure(const std::shared_ptr<PendingSwitch> &state);
    // 发布「已切到这条」：位置归零、重置其他条目进度、回调 TC/DC/PC。
    void publishSelection(const MusicPropertiesPtr &musicProperties);
    // 作废 pending 并通知播放器清门闩（stop / clearPlaylist / 删除目标曲目 / 析构）。
    // 析构路径传 notify=false：回调对象可能已半析构，只作废不通知。
    void cancelPendingSwitch(bool notify = true);
    MusicPropertiesPtr findTrackByUid(uint64_t uid) const;
    // 入列的唯一实体：扫描与 addTrack 共用，差别只在「用户显式命令」这一路上的失败表达
    // 与拒绝累计（契约 §4.1 失败三类）。返回 nullptr 表示该条目没有进列表。
    MusicPropertiesPtr addMusicInternal(const std::string &musicPath, bool userInitiated);
    // 补位序（ADR 0018 §1）：只算候选位置，不产生副作用。
    // Sequential / Loop / 单曲模式 → 从 p 起的环形顺序；Random → 剩余条目的随机全序。
    std::vector<size_t> seatingOrder(size_t p);
    // 按序试播：跳过已失败条目、逐个 beginSwitch，失败即记账并继续；命中
    //（Committed / Pending）返回 true。skipToNextPlayable 走 AutoSkip（失败落
    // Paused / Stopped），removeTrack 的补位走 SeatReplacement（失败等价
    // CurrentCleared）；pending 失败的候选续做也复用同一份（ADR 0020 决策 4）。
    bool selectFirstPlayable(const std::vector<size_t> &order, SwitchOrigin origin);
    // 无条件发布全量列表（列表编辑命令专用）。扫描路径仍走 _updateList() 的空列表守卫。
    void publishPlaylist();
    // 换任务：旧任务只 abort、移入退役槽（不 join），再为新曲造 ring + decoder 并 start()。
    // 退役任务的 join 推迟到退役线程退出后的消费拍 / 下一次切歌 / stop / 析构（规格 §5）。
    bool replacePlaybackTask(const MusicPropertiesPtr &musicProperties);
    // 把当前任务移入退役槽：只置取消位，不 join；槽满时先 join 上一个退役任务。
    void retireCurrentTask();
    // 回收退役槽：同步 join 并释放。调用点：下一次切歌 / stop / 析构，
    // 以及退役线程退出后的消费拍（reapRetiredTaskIfFinished）。
    void joinRetiredTask();
    // 只清位置 / 数据量这类元数据；任务（ring / decoder）不属于曲目，不在遍历范围内。
    void resetTrackProgressExcept(uint64_t keepUid);
    void reportError(ErrorCode code, const std::string &detail,
                     const MusicPropertiesPtr &musicProperties, bool userInitiated = false);
    // 播放期重开失败（规格 player-state-contract.md §4.1）：一次 onError、记账为失败曲目，
    // 不改播放状态、不进扫描汇总。index 用条目在列表中的下标，不是 -1。
    void reportPlaybackOpenFailure(const MusicPropertiesPtr &musicProperties,
                                   sdk_utils::status_t status);
    void recordScanRejection(ErrorCode code);
    // 打开失败成因（#278）：与 m_scanRejects 同锁，只在入列期记账。
    void recordScanRejectCause(const char *cause);
    std::string currentTraceId() const;

private:
    std::atomic<MusicPlaybackMode> m_playbackMode;
    // uid 单调递增、不复用；只在队列线程写。
    uint64_t m_nextUid;
    std::mt19937 m_randomEngine;
    // 当前播放任务。只有控制线程写，消费者读快照；锁只保护指针本身，ring 内部无锁。
    mutable std::mutex m_taskMutex;
    PlaybackTaskPtr m_currentTask;
    // 单槽退役：切歌时旧任务在这里等下一次控制事件 join（规格 §5.1）。
    PlaybackTaskPtr m_retiredTask;
    // 切歌重开执行器（ADR 0020）：所有 acquire() 都在这条线程上执行。
    std::unique_ptr<SwitchOpenExecutor> m_openExecutor;
    // 等待期的请求状态。控制线程独占读写；IO 线程只写自己那一份结果，
    // 由 runBounded 的完成标志 / 回投任务建立可见性。
    std::shared_ptr<PendingSwitch> m_pendingSwitch;
    // switchPending() 的跨线程镜像（测试与播放器在控制线程外查询）。
    std::atomic<bool> m_switchPending;
    // 切换意图代际：每次 beginSwitch 递增；晚到结果代际不符即静默丢弃。
    uint64_t m_switchGeneration;
    mutable std::mutex m_failedMutex;
    // 本次播放会话内失败的**条目**身份（ADR 0018 §2）：按 uid 而不是 index 记账，
    // 压缩重排不会让「已知坏曲」漂移到别人头上；也不能按 TrackInfo::id——
    // id 不唯一，一条坏文件会毒化同路径的所有副本。
    std::set<uint64_t> m_failedUids;
    mutable std::mutex m_rejectMutex;
    std::map<ErrorCode, int> m_scanRejects;
    // 打开失败的成因分布（#278）：只用于追加一条汇总日志，不改既有聚合键与文案。
    std::map<std::string, int> m_scanRejectCauses;
};
}
