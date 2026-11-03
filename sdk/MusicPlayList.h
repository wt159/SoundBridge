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
    std::atomic<uint64_t> curPositionMs;
    off64_t dataSize;
    std::atomic<off64_t> curDataOffset;
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
    std::shared_ptr<AudioRingBuffer> ringBuffer;
    std::shared_ptr<AudioStreamDecoder> streamDecoder;
};

using MusicPropertiesPtr = std::shared_ptr<MusicProperties>;

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
    bool startStreaming(const MusicPropertiesPtr &musicProperties);
    void releaseDecodedBuffersExcept(size_t keepIndex, size_t keepIndex2);
    void reportError(ErrorCode code, const std::string &detail,
                     const MusicPropertiesPtr &musicProperties);
    void recordScanRejection(ErrorCode code);
    std::string currentTraceId() const;

private:
    std::atomic<MusicPlaybackMode> m_playbackMode;
    std::mt19937 m_randomEngine;
    mutable std::mutex m_failedMutex;
    std::set<size_t> m_failedIndices;
    mutable std::mutex m_rejectMutex;
    std::map<ErrorCode, int> m_scanRejects;
};
}
