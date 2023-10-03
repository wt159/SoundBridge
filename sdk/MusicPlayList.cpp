#include "ExtractorFactory.h"
#include "MusicPlayList.h"
#include "LogWrapper.h"
#include "type_name.hpp"

namespace sdk {

#define LOG_TAG "MusicPlayList"

MusicPlayList::MusicPlayList(MusicPlayListCallback *callback)
    : m_callback(callback)
    , m_curIndex(0)
    , m_musicListProperties()
{
    m_musicListProperties.clear();
    LOG_INFO(LOG_TAG, "MusicPlayList construct");
}
MusicPlayList::MusicPlayList(MusicPlayListCallback *callback, std::vector<std::string> &musicList)
    : m_callback(callback)
    , m_curIndex(0)
    , m_musicListProperties()
{
    m_musicListProperties.clear();
    for (auto &path : musicList) {
        LOG_DEBUG(LOG_TAG, "addMusic : %s", path.data());
        m_workQueue.asyncRunTask([this, path]() { _addMusic(path); });
    }
}
MusicPlayList::~MusicPlayList() { }
void MusicPlayList::addMusic(const std::string &musicPath)
{
    LOG_INFO(LOG_TAG, "addMusic : %s", musicPath.data());
    m_workQueue.asyncRunTask([this, musicPath]() { _addMusic(musicPath); });
}
void MusicPlayList::next()
{
    m_workQueue.asyncRunTask([this]() { _next(); });
}
void MusicPlayList::pervious()
{
    m_workQueue.asyncRunTask([this]() { _pervious(); });
}
void MusicPlayList::setCurrentIndex(int index)
{
    m_workQueue.asyncRunTask([this, index]() { _setCurrentIndex(index); });
}
int MusicPlayList::getMusicCount()
{
    return m_curIndex.load();
}
void MusicPlayList::_addMusic(const std::string &musicPath)
{
    MusicPropertiesPtr musicProperties   = std::make_shared<MusicProperties>();
    musicProperties->index               = m_curIndex++;
    FileProperties &fileProperties       = musicProperties->fileProperties;
    SignalProperties &signalProperties   = musicProperties->signalProperties;
    ProcessProperties &processProperties = musicProperties->processProperties;
    fileProperties.fullPath              = musicPath;
    fileProperties.parseFileName();
    LOG_INFO(LOG_TAG, "fineName      : %s", fileProperties.fileName.data());
    LOG_INFO(LOG_TAG, "extensionName : %s", fileProperties.extensionName.data());
    FileSource *source = new FileSource(musicPath.c_str());
    if(source == nullptr) {
        LOG_ERROR(LOG_TAG, "new FileSource failed");
        return;
    }
    processProperties.source       = source;
    ExtractorHelper *extractor
        = ExtractorFactory::createExtractor(source, fileProperties.extensionName);
    if(extractor == nullptr) {
        LOG_ERROR(LOG_TAG, "createExtractor failed");
        return;
    }
    processProperties.extractor    = extractor;
    signalProperties.durationMs    = extractor->getDurationMs();
    signalProperties.curPositionMs = 0;
    signalProperties.dataSize      = extractor->getDataSize();
    signalProperties.curDataOffset = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    processProperties.resample     = nullptr;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    LOG_INFO(LOG_TAG, "dataSize      : %lld", signalProperties.dataSize);
    char *buf                      = new char[signalProperties.dataSize];
    if(buf == nullptr) {
        LOG_ERROR(LOG_TAG, "new buf failed");
        return;
    }
    extractor->readAudioRawData(0, signalProperties.dataSize, buf);
    musicProperties->rawBuffer
        = std::make_shared<AudioBuffer>(AudioBuffer(buf, signalProperties.dataSize));
    if(musicProperties->rawBuffer == nullptr) {
        LOG_ERROR(LOG_TAG, "new AudioBuffer failed");
        return;
    }
    m_musicListProperties.push_back(musicProperties);
    if (m_selectMusicProperties == nullptr) {
        m_selectMusicProperties = musicProperties;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    }
}
void MusicPlayList::_next()
{
    if (m_curIndex.load() == 0)
        return;
    size_t index            = (m_selectMusicProperties->index + 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_pervious()
{
    if (m_curIndex.load() == 0)
        return;
    size_t index            = (m_selectMusicProperties->index - 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_setCurrentIndex(int index)
{
    if (m_curIndex.load() == 0)
        return;
    m_selectMusicProperties = m_musicListProperties.at(index);
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
}
