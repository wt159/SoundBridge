#include "MusicPlayList.h"
#include "ExtractorFactory.h"
#include "LogApi.h"
#include "LogWrapper.h"
#include "Metrics.h"
#include "type_name.hpp"
#include <chrono>
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
void MusicPlayList::setTraceId(const std::string &traceId)
{
    std::lock_guard<std::mutex> lock(m_traceMutex);
    m_traceId = traceId;
}

bool MusicPlayList::skipToNextPlayable()
{
    if (m_curIndex.load() == 0 || m_selectMusicProperties == nullptr) {
        return false;
    }
    size_t count      = m_curIndex.load();
    size_t startIndex = m_selectMusicProperties->index;
    size_t prevIndex  = startIndex;
    for (size_t step = 1; step <= count; ++step) {
        size_t index                 = (startIndex + step) % count;
        MusicPropertiesPtr candidate = m_musicListProperties.at(index);
        if (!ensureDecoded(candidate)) {
            continue;
        }
        releaseDecodedBuffersExcept(candidate->index, prevIndex);
        candidate->signalProperties.curDataOffset = 0;
        candidate->signalProperties.curPositionMs = 0;
        m_selectMusicProperties                   = candidate;
        m_callback->putMusicPlayListCurBuf(m_selectMusicProperties);
        return true;
    }
    return false;
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
        reportError(ErrorCode::FileOpenFailed, "new FileSource failed", musicProperties);
        return;
    }
    if (source->initCheck() != OK) {
        reportError(ErrorCode::FileOpenFailed, "FileSource initCheck failed", musicProperties);
        return;
    }
    processProperties.source = source;
    std::shared_ptr<ExtractorHelper> extractor(
        ExtractorFactory::createExtractor(source.get(), fileProperties.extensionName, true));
    if (extractor == nullptr) {
        reportError(ErrorCode::ExtractorUnsupported, "createExtractor returned nullptr",
                    musicProperties);
        return;
    }
    if (extractor->initCheck() != OK) {
        reportError(ErrorCode::ExtractorInitFailed, "extractor initCheck failed", musicProperties);
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
    signalProperties.curDataOffset = 0;
    signalProperties.spec          = extractor->getAudioSpec();
    signalProperties.durationMs    = signalProperties.spec.durationMs;
    signalProperties.dataSize      = 0;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    m_musicListProperties.push_back(musicProperties);
    m_curIndex++;
    if (m_selectMusicProperties == nullptr) {
        if (!ensureDecoded(musicProperties)) {
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
    MusicPropertiesPtr prev = m_selectMusicProperties;
    size_t prevIndex        = prev ? prev->index : SIZE_MAX;
    size_t index            = (m_selectMusicProperties->index + 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        m_selectMusicProperties = prev;
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
    MusicPropertiesPtr prev = m_selectMusicProperties;
    size_t prevIndex        = prev ? prev->index : SIZE_MAX;
    size_t index            = (m_selectMusicProperties->index - 1) % m_curIndex.load();
    m_selectMusicProperties = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        m_selectMusicProperties = prev;
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
    MusicPropertiesPtr prev = m_selectMusicProperties;
    size_t prevIndex        = prev ? prev->index : SIZE_MAX;
    m_selectMusicProperties = m_musicListProperties.at(index);
    if (!ensureDecoded(m_selectMusicProperties)) {
        m_selectMusicProperties = prev;
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
        Metrics::RecordCount("buffer_hit", 1,
                             MetricTags { currentTraceId(),
                                          musicProperties->fileProperties.fullPath,
                                          static_cast<int>(musicProperties->index) });
        return true;
    }
    Metrics::RecordCount("buffer_miss", 1,
                         MetricTags { currentTraceId(), musicProperties->fileProperties.fullPath,
                                      static_cast<int>(musicProperties->index) });

    ProcessProperties &processProperties = musicProperties->processProperties;
    if (processProperties.extractor == nullptr) {
        reportError(ErrorCode::ExtractorInitFailed, "extractor is nullptr", musicProperties);
        return false;
    }

    int ret          = 0;
    auto decodeStart = std::chrono::steady_clock::now();
    std::shared_ptr<AudioDecodeProcess> decode(
        new AudioDecodeProcess(processProperties.extractor.get()));
    if (decode == nullptr || decode->initCheck() != OK) {
        reportError(ErrorCode::DecodeInitFailed, "AudioDecodeProcess initCheck failed",
                    musicProperties);
        return false;
    }
    processProperties.decode = decode;

    SignalProperties &signalProperties    = musicProperties->signalProperties;
    signalProperties.spec                 = decode->getDecodeSpec();
    AudioBuffer::AudioBufferPtr decBufPtr = decode->getDecodeBuffer();
    if (decBufPtr == nullptr) {
        reportError(ErrorCode::DecodeFailed, "decode buffer is null", musicProperties);
        return false;
    }
    auto decodeEnd = std::chrono::steady_clock::now();
    auto decodeMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
    MetricTags tags { currentTraceId(), musicProperties->fileProperties.fullPath,
                      static_cast<int>(musicProperties->index) };
    Metrics::RecordTiming("decode_total_ms", static_cast<uint64_t>(decodeMs), tags);
    Metrics::RecordTiming("first_frame_ms", static_cast<uint64_t>(decodeMs), tags);
    signalProperties.dataSize   = decBufPtr->size();
    signalProperties.durationMs = signalProperties.spec.durationMs;
    LOG_INFO(LOG_TAG, "durationMs    : %llu", signalProperties.durationMs);
    LOG_INFO(LOG_TAG, "dataSize      : %lld", signalProperties.dataSize);
    if (signalProperties.durationMs > 0) {
        double durationSec       = static_cast<double>(signalProperties.durationMs) / 1000.0;
        double avgDecodeMsPerSec = static_cast<double>(decodeMs) / durationSec;
        Metrics::RecordGauge("decode_ms_per_sec", avgDecodeMsPerSec, tags, "ms/s");
    }
    if (decBufPtr->size() > 0) {
        double mb = static_cast<double>(decBufPtr->size()) / (1024.0 * 1024.0);
        if (mb > 0.0) {
            double avgDecodeMsPerMb = static_cast<double>(decodeMs) / mb;
            Metrics::RecordGauge("decode_ms_per_mb", avgDecodeMsPerMb, tags, "ms/mb");
        }
    }

    if (signalProperties.spec == m_devSpec) {
        LOG_INFO(LOG_TAG, "audio spec is same");
        processProperties.resample = nullptr;
        musicProperties->rawBuffer = decBufPtr;
        return true;
    }

    LOG_INFO(LOG_TAG, "audio spec is not same, need resample");
    AudioSpec inSpec  = signalProperties.spec;
    AudioSpec outSpec = m_devSpec;
    LOGD("in  spec %d %d %d", inSpec.sampleRate, inSpec.numChannel, inSpec.bytesPerSample);
    LOGD("out spec %d %d %d", outSpec.sampleRate, outSpec.numChannel, outSpec.bytesPerSample);
    inSpec.samples             = 1024;
    processProperties.resample = std::make_shared<AudioResample>(inSpec, outSpec);
    if (processProperties.resample == nullptr || processProperties.resample->initCheck() != OK) {
        reportError(ErrorCode::ResampleInitFailed, "AudioResample initCheck failed",
                    musicProperties);
        return false;
    }
    LOG_INFO(LOG_TAG, "resampleBufSize : %lu", decBufPtr->size());
    long resampleBufSize = (double)((double)decBufPtr->size() * (double)outSpec.sampleRate
                                    / (double)inSpec.sampleRate);
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
    size_t outSize = 0, outOnceSize = resampleBufSize;
    auto resampleStart = std::chrono::steady_clock::now();
    while (1) {
        ret = processProperties.resample->resample(decOutputBuf + inSize, inOnceSize,
                                                   resampleBuf + outSize, &outOnceSize);
        if (ret < 0) {
            reportError(ErrorCode::ResampleFailed, "resample failed", musicProperties);
            return false;
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
    auto resampleEnd = std::chrono::steady_clock::now();
    auto resampleMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(resampleEnd - resampleStart)
              .count();
    Metrics::RecordTiming("resample_ms", static_cast<uint64_t>(resampleMs), tags);
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
