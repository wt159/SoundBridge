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
#include "WorkQueue.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace sdk {

struct FileProperties {
    std::string fullPath;
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
    FileProperties fileProperties;
    SignalProperties signalProperties;
    ProcessProperties processProperties;
};

using MusicPropertiesPtr = std::shared_ptr<MusicProperties>;

// 一次播放任务：ring 与 decoder 由**当前曲目独占**，跨曲目不复用（规格 #55 第 4.3 节）。
//
// - 创建者：播放列表在做「这次要播哪首」的决定时造；消费者只借指针，不持有所有权。
// - 销毁时机：新任务 start() **之前** —— stop / 切歌 / 停止播放都把旧任务整个丢掉，
//   旧 ring 的引用计数归零，旧 decoder 析构时 join 掉自己的解码线程。
// - 成员声明顺序决定析构顺序：decoder 声明在 ring 之后，所以先析构 decoder（其析构函数
//   join 掉自己的解码线程），再释放它只持有裸指针的 ring。
struct PlaybackTask {
    PlaybackTask(const std::shared_ptr<AudioRingBuffer> &ringBuffer,
                 const std::shared_ptr<AudioStreamDecoder> &streamDecoder, size_t track)
        : ring(ringBuffer)
        , decoder(streamDecoder)
        , trackIndex(track)
    {
    }

    std::shared_ptr<AudioRingBuffer> ring;
    std::shared_ptr<AudioStreamDecoder> decoder;
    size_t trackIndex;
};

using PlaybackTaskPtr = std::shared_ptr<PlaybackTask>;

class MusicPlayListCallback {
public:
    virtual ~MusicPlayListCallback()                                    = default;
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property)    = 0;
    virtual void updateMusicList(std::vector<MusicPropertiesPtr> &list) = 0;
    virtual void onMusicPlayListError(ErrorCode code, const std::string &detail, int index,
                                      const std::string &path)
        = 0;
};

class MusicPlayList : public NonCopyable {
private:
    MusicPlayListCallback *m_callback;
    std::atomic<size_t> m_curIndex;
    std::atomic<size_t> m_selectIndex;
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
    // 为「当前曲目」准备播放任务。已经有一个活着的任务（暂停后恢复）就复用；
    // 没有 / 已结束 / 已失败则先 stop+join 旧任务，再建新 ring + 新 decoder。
    bool startCurrentTask();
    // 丢弃当前任务：先 join 解码线程，再销毁 ring（旧 PCM 一律丢弃、不 drain）。
    void stopCurrentTask();

    // 本次播放会话内因坏数据失败过的曲目：自动跳过不再回头重试（同一曲不重试第二次）。
    // 用户按播放（MusicPlayer::play）时清空，视为新会话。
    void markTrackFailed(size_t index);
    bool isTrackFailed(size_t index) const;
    void clearFailedTracks();

    // 批量扫描汇总：被拒文件不进播放列表，永远不会成为「当前曲目」，
    // 只能靠这份按错误码分桶的汇总把它们报给 UI（规格 5.4 H6）。
    void reportScanSummary();

protected:
    void _addMusic(const std::string &musicPath);
    void _next();
    void _pervious();
    void _setCurrentIndex(int index);
    void _updateList();

private:
    bool selectTrack(size_t index);
    // 换任务：stop+join 旧任务并销毁旧 ring，再为新曲造 ring + decoder 并 start()。
    // 全程在调用它的控制线程上完成（WorkQueue 单线程串行），禁止 detach / 后台 join。
    bool replacePlaybackTask(const MusicPropertiesPtr &musicProperties);
    // 只清位置 / 数据量这类元数据；任务（ring / decoder）不属于曲目，不在遍历范围内。
    void resetTrackProgressExcept(size_t keepIndex);
    void reportError(ErrorCode code, const std::string &detail,
                     const MusicPropertiesPtr &musicProperties);
    void recordScanRejection(ErrorCode code);
    std::string currentTraceId() const;

private:
    std::atomic<MusicPlaybackMode> m_playbackMode;
    std::mt19937 m_randomEngine;
    // 当前播放任务。只有控制线程写，消费者读快照；锁只保护指针本身，ring 内部无锁。
    mutable std::mutex m_taskMutex;
    PlaybackTaskPtr m_currentTask;
    mutable std::mutex m_failedMutex;
    std::set<size_t> m_failedIndices;
    mutable std::mutex m_rejectMutex;
    std::map<ErrorCode, int> m_scanRejects;
};
}
