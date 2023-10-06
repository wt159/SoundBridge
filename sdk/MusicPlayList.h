#pragma once

#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioResample.h"
#include "ExtractorHelper.hpp"
#include "AudioDecodeProcess.h"
#include "FileSource.h"
#include "NonCopyable.hpp"
#include "WorkQueue.hpp"
#include <atomic>
#include <memory>
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
        // 查找最后一个斜杠的位置
        size_t lastSlashPos = fullPath.find_last_of("/");
        if (lastSlashPos != std::string::npos) {
            // 提取文件目录
            fileDir = fullPath.substr(0, lastSlashPos + 1);
            // 提取文件名
            fileName = fullPath.substr(lastSlashPos + 1);
            // 查找最后一个点的位置
            size_t lastDotPos = fileName.find_last_of(".");
            if (lastDotPos != std::string::npos) {
                // 提取扩展名
                extensionName = fileName.substr(lastDotPos);
                // 去除扩展名后的文件名
                fileName = fileName.substr(0, lastDotPos);
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
    std::shared_ptr<AudioDecodeProcess> decode;
    std::shared_ptr<AudioResample> resample;
};

struct MusicProperties {
    size_t index;
    FileProperties fileProperties;
    SignalProperties signalProperties;
    ProcessProperties processProperties;
    AudioBuffer::AudioBufferPtr rawBuffer;

    ~MusicProperties() { processProperties.~ProcessProperties(); }
};

using MusicPropertiesPtr = std::shared_ptr<MusicProperties>;

class MusicPlayListCallback {
public:
    virtual ~MusicPlayListCallback()                                 = default;
    virtual void putMusicPlayListCurBuf(MusicPropertiesPtr property) = 0;
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

public:
    MusicPlayList() = delete;
    MusicPlayList(MusicPlayListCallback *callback, WorkQueue *wq, AudioSpec &devSpec);
    ~MusicPlayList();

    void addMusic(const std::string &musicPath);
    void next();
    void pervious();
    void setCurrentIndex(int index);
    int getMusicCount();

protected:
    void _addMusic(const std::string &musicPath);
    void _next();
    void _pervious();
    void _setCurrentIndex(int index);
};
}
