#include "ExtractorFactory.h"
#include "LogWrapper.h"
#include "MusicPlayList.h"
#include "type_name.hpp"
#include <cstddef>

namespace sdk {

#define LOG_TAG "MusicPlayList"

using namespace sdk_utils;

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
void MusicPlayList::_addMusic(const std::string &musicPath)
{
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
        LOG_ERROR(LOG_TAG, "new FileSource failed");
        return;
    }
    processProperties.source = source;
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(source.get(), fileProperties.extensionName, true));
    if (extractor == nullptr || extractor->initCheck() != OK) {
        LOG_ERROR(LOG_TAG, "createExtractor failed or initCheck failed");
        return;
    }
    processProperties.extractor = extractor;
    signalProperties.curPositionMs = 0;
    signalProperties.curDataOffset = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    signalProperties.durationMs    = signalProperties.spec.durationMs;
    signalProperties.dataSize      = 0;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    m_musicListProperties.push_back(musicProperties);
    m_curIndex++;
    if (m_selectMusicProperties == nullptr) {
        if (!ensureDecoded(musicProperties)) {
            LOG_ERROR(LOG_TAG, "ensureDecoded failed for first track");
            return;
        }
        m_selectMusicProperties = musicProperties;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
    }
    LOGI("this music fished, m_curIndex=%d", m_curIndex.load());
}
void MusicPlayList::_next()
{
    if (m_curIndex.load() == 0)
        return;
    size_t prevIndex = m_selectMusicProperties ? m_selectMusicProperties->index : SIZE_MAX;
    size_t index            = (m_selectMusicProperties->index + 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        LOG_ERROR(LOG_TAG, "ensureDecoded failed for next track");
        return;
    }
    releaseDecodedBuffersExcept(m_selectMusicProperties->index, prevIndex);
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_pervious()
{
    if (m_curIndex.load() == 0)
        return;
    size_t prevIndex = m_selectMusicProperties ? m_selectMusicProperties->index : SIZE_MAX;
    size_t index            = (m_selectMusicProperties->index - 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        LOG_ERROR(LOG_TAG, "ensureDecoded failed for previous track");
        return;
    }
    releaseDecodedBuffersExcept(m_selectMusicProperties->index, prevIndex);
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_setCurrentIndex(int index)
{
    if (m_curIndex.load() == 0)
        return;
    size_t prevIndex = m_selectMusicProperties ? m_selectMusicProperties->index : SIZE_MAX;
    m_selectMusicProperties                                 = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        LOG_ERROR(LOG_TAG, "ensureDecoded failed for selected track");
        return;
    }
    releaseDecodedBuffersExcept(m_selectMusicProperties->index, prevIndex);
    m_selectMusicProperties->signalProperties.curDataOffset = 0;
    m_selectMusicProperties->signalProperties.curPositionMs = 0;
    m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
}
void MusicPlayList::_updateList()
{
    if (m_musicListProperties.size() > 0) {
        m_callback->updateMusicList(m_musicListProperties);
    }
}

bool MusicPlayList::ensureDecoded(const MusicPropertiesPtr &musicProperties)
{
    if (musicProperties == nullptr) {
        return false;
    }
    if (musicProperties->rawBuffer != nullptr) {
        return true;
    }

    ProcessProperties &processProperties = musicProperties->processProperties;
    if (processProperties.extractor == nullptr) {
        LOG_ERROR(LOG_TAG, "extractor is nullptr, can not decode");
        return false;
    }

    int ret = 0;
    std::shared_ptr<AudioDecodeProcess> decode(
        new AudioDecodeProcess(processProperties.extractor.get()));
    if (decode == nullptr || decode->initCheck() != OK) {
        LOG_ERROR(LOG_TAG, "new AudioDecodeProcess failed or initCheck failed, %p", decode.get());
        return false;
    }
    processProperties.decode = decode;

    SignalProperties &signalProperties    = musicProperties->signalProperties;
    signalProperties.spec                 = decode->getDecodeSpec();
    AudioBuffer::AudioBufferPtr decBufPtr = decode->getDecodeBuffer();
    signalProperties.dataSize             = decBufPtr->size();
    signalProperties.durationMs           = signalProperties.spec.durationMs;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    LOG_INFO(LOG_TAG, "dataSize      : %lld", signalProperties.dataSize);

    if (signalProperties.spec == m_devSpec) {
        LOG_INFO(LOG_TAG, "audio spec is same");
        processProperties.resample = nullptr;
        musicProperties->rawBuffer = decBufPtr;
        return true;
    }

    LOG_INFO(LOG_TAG, "audio spec is not same, need resample");
    AudioSpec inSpec           = signalProperties.spec;
    AudioSpec outSpec          = m_devSpec;
    LOGD("in  spec %d %d %d", inSpec.sampleRate, inSpec.numChannel, inSpec.bytesPerSample);
    LOGD("out spec %d %d %d", outSpec.sampleRate, outSpec.numChannel, outSpec.bytesPerSample);
    inSpec.samples             = 1024;
    processProperties.resample = std::make_shared<AudioResample>(inSpec, outSpec);
    if (processProperties.resample == nullptr || processProperties.resample->initCheck() != OK) {
        LOG_ERROR(LOG_TAG, "new AudioResample failed");
        return false;
    }
    LOG_INFO(LOG_TAG, "resampleBufSize : %lu", decBufPtr->size());
    long resampleBufSize = (double)((double)decBufPtr->size() * (double)outSpec.sampleRate / (double)inSpec.sampleRate);
    LOG_INFO(LOG_TAG, "resampleBufSize : %lu", resampleBufSize);
    resampleBufSize = resampleBufSize * outSpec.numChannel / inSpec.numChannel;
    LOG_INFO(LOG_TAG, "resampleBufSize : %lu", resampleBufSize);
    resampleBufSize = resampleBufSize * outSpec.bytesPerSample / inSpec.bytesPerSample;
    LOG_INFO(LOG_TAG, "resampleBufSize : %lu", resampleBufSize);

    AudioBuffer::AudioBufferPtr resampleBufPtr(new AudioBuffer(resampleBufSize));
    char *resampleBuf  = resampleBufPtr->data();
    char *decOutputBuf = decBufPtr->data();
    size_t inOnceSize  = inSpec.samples * inSpec.numChannel * inSpec.bytesPerSample;
    LOG_INFO(LOG_TAG, "inOnceSize : %d", inOnceSize);
    size_t inSize  = 0;
    size_t outSize = 0, outOnceSize = 0;
    while (1) {
        ret = processProperties.resample->resample(decOutputBuf + inSize, inOnceSize,
                                                   resampleBuf + outSize, &outOnceSize);
        if (ret < 0) {
            LOG_ERROR(LOG_TAG, "resample failed");
            break;
        }
        inSize  += inOnceSize;
        outSize += outOnceSize;
        if ((inSize + inOnceSize) > decBufPtr->size()) {
            inOnceSize  = decBufPtr->size() - inSize;
            outOnceSize = resampleBufSize - outSize;
            LOG_DEBUG(LOG_TAG, "inOnceSize : %d, outOnceSize : %d", inOnceSize, outOnceSize);
        }
        if (inSize >= decBufPtr->size()) {
            break;
        }
    }
    LOG_INFO(LOG_TAG, "resampleBufSize : %llu", resampleBufSize);
    signalProperties.dataSize  = outSize;
    musicProperties->rawBuffer = resampleBufPtr;
    return musicProperties->rawBuffer != nullptr;
}

void MusicPlayList::releaseDecodedBuffersExcept(size_t keepIndex, size_t keepIndex2)
{
    for (auto &item : m_musicListProperties) {
        if (item == nullptr) {
            continue;
        }
        if (item->index == keepIndex || item->index == keepIndex2) {
            continue;
        }
        item->rawBuffer.reset();
        item->processProperties.decode.reset();
        item->processProperties.resample.reset();
        item->signalProperties.curDataOffset = 0;
        item->signalProperties.curPositionMs = 0;
        item->signalProperties.dataSize      = 0;
    }
}
}

