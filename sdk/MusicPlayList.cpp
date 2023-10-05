#include "ExtractorFactory.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "type_name.hpp"

namespace sdk {

#define LOG_TAG "MusicPlayList"

MusicPlayList::MusicPlayList(MusicPlayListCallback *callback, WorkQueue *wq, AudioSpec &devSpec)
    : m_callback(callback)
    , m_curIndex(0)
    , m_workQueue(wq)
    , m_devSpec(devSpec)
    , m_musicListProperties()
{
    m_musicListProperties.clear();
    LOG_INFO(LOG_TAG, "MusicPlayList construct");
}

MusicPlayList::~MusicPlayList() { }

void MusicPlayList::addMusic(const std::string &musicPath)
{
    LOG_INFO(LOG_TAG, "addMusic : %s", musicPath.data());
    m_workQueue->asyncRunTask([this, musicPath]() { _addMusic(musicPath); });
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
int MusicPlayList::getMusicCount()
{
    return m_curIndex.load();
}
void MusicPlayList::_addMusic(const std::string &musicPath)
{
    int ret                              = 0;
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
    if (source == nullptr) {
        LOG_ERROR(LOG_TAG, "new FileSource failed");
        return;
    }
    processProperties.source = source;
    ExtractorHelper *extractor
        = ExtractorFactory::createExtractor(source, fileProperties.extensionName);
    if (extractor == nullptr) {
        LOG_ERROR(LOG_TAG, "createExtractor failed");
        return;
    }
    processProperties.extractor    = extractor;
    signalProperties.durationMs    = extractor->getDurationMs();
    signalProperties.curPositionMs = 0;
    signalProperties.dataSize      = extractor->getDataSize();
    signalProperties.curDataOffset = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    LOG_INFO(LOG_TAG, "dataSize      : %lld", signalProperties.dataSize);
    AudioBuffer::AudioBufferPtr extBufPtr;
    extractor->readAudioRawData(extBufPtr);

    if (signalProperties.spec == m_devSpec) {
        LOG_INFO(LOG_TAG, "audio spec is same");
        processProperties.resample = nullptr;
        musicProperties->rawBuffer = extBufPtr;
    } else {
        LOG_INFO(LOG_TAG, "audio spec is not same, need resample");
        AudioSpec inSpec           = signalProperties.spec;
        AudioSpec outSpec          = m_devSpec;
        inSpec.samples             = 1024;
        processProperties.resample = new AudioResample(inSpec, outSpec);
        if(processProperties.resample == nullptr) {
            LOG_ERROR(LOG_TAG, "new AudioResample failed");
            return;
        }
        size_t resampleBufSize     = extBufPtr->size() * outSpec.sampleRate * outSpec.numChannel
            * outSpec.bytesPerSample / inSpec.sampleRate / inSpec.numChannel
            / inSpec.bytesPerSample;
        LOG_INFO(LOG_TAG, "resampleBufSize : %d", resampleBufSize);

        AudioBuffer::AudioBufferPtr resampleBufPtr(new AudioBuffer(resampleBufSize));
        char *resampleBuf = resampleBufPtr->data();
        char *extractorOutputBuf = extBufPtr->data();
        size_t inOnceSize = inSpec.samples * inSpec.numChannel * inSpec.bytesPerSample;
        LOG_INFO(LOG_TAG, "inOnceSize : %d", inOnceSize);
        size_t inSize  = 0;
        size_t outSize = 0, outOnceSize = 0;
        while (1) {
            ret = processProperties.resample->resample(extractorOutputBuf + inSize, inOnceSize,
                                                       resampleBuf + outSize, &outOnceSize);
            if (ret < 0) {
                LOG_ERROR(LOG_TAG, "resample failed");
                break;
            }
            inSize  += inOnceSize;
            outSize += outOnceSize;
            if ((inSize + inOnceSize) > extBufPtr->size()) {
                inOnceSize  = extBufPtr->size() - inSize;
                outOnceSize = resampleBufSize - outSize;
                LOG_DEBUG(LOG_TAG, "inOnceSize : %d, outOnceSize : %d", inOnceSize, outOnceSize);
            }
            if (inSize >= extBufPtr->size()) {
                break;
            }
        }
        LOG_INFO(LOG_TAG, "resampleBufSize : %llu", resampleBufSize);
        signalProperties.dataSize = outSize;
        musicProperties->rawBuffer = resampleBufPtr;
    }

    if (musicProperties->rawBuffer == nullptr) {
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
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_pervious()
{
    if (m_curIndex.load() == 0)
        return;
    size_t index            = (m_selectMusicProperties->index - 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_setCurrentIndex(int index)
{
    if (m_curIndex.load() == 0)
        return;
    m_selectMusicProperties                                 = m_musicListProperties.at(index);
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
}
