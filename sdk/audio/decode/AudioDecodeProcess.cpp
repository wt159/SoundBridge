#include "AudioDecodeProcess.h"
#include "LogWrapper.h"

#define LOG_TAG "AudioDecodeProcess"

using namespace sdk_utils;

AudioDecodeProcess::AudioDecodeProcess(ExtractorHelper *extractor)
    : m_extractor(extractor)
    , m_decode(nullptr)
    , m_decSize(0)
{
    m_decBufVec.clear();
    m_spec    = m_extractor->getAudioSpec();
    m_codecID = m_extractor->getAudioCodecID();
    LOG_INFO(LOG_TAG, "m_codecID: %#x", m_codecID);
    m_initCheck = init();
    if (m_initCheck < OK) {
        LOG_ERROR(LOG_TAG, "init failed");
    }
}

AudioDecodeProcess::~AudioDecodeProcess() { }

void AudioDecodeProcess::onAudioDecodeCallback(AudioDecodeSpec &out)
{
    size_t size = out.spec.samples * out.spec.numChannel * out.spec.bytesPerSample;
    AudioBuffer::AudioBufferPtr buf = std::make_shared<AudioBuffer>(size);
    off64_t offset                  = 0;
    for (int i = 0; i < out.spec.samples; i++) {
        for (int ch = 0; ch < out.spec.numChannel; ch++) {
            buf->setData(offset, out.spec.bytesPerSample,
                         (char *)out.lineData[ch] + out.spec.bytesPerSample * i);
            offset += out.spec.bytesPerSample;
        }
    }
    m_spec     = out.spec;
    m_decSize += offset;
    m_decBufVec.push_back(buf);
}

status_t AudioDecodeProcess::init()
{
    AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
    if (extPtr == nullptr) {
        LOGE("getMetaData failed");
        return INVALID_OPERATION;
    }
    if (m_codecID == AUDIO_CODEC_ID_NONE) {
        LOG_ERROR(LOG_TAG, "m_codecID is AUDIO_CODEC_ID_NONE, not need decode");
        m_decBuf  = extPtr;
        m_decSize = m_decBuf->size();
    } else {
        m_decode = std::make_shared<AudioDecode>(m_codecID, this);
        if (m_decode == nullptr || m_decode->initCheck() != OK) {
            LOG_ERROR(LOG_TAG, "new AudioDecode failed or initCheck failed, %p", m_decode.get());
            return INVALID_OPERATION;
        }
        LOG_INFO(LOG_TAG, "extractor buffer size(): %llu", extPtr->size());
        int ret = m_decode->decode(extPtr->data(), extPtr->size());
        if (ret < 0) {
            LOG_ERROR(LOG_TAG, "decode failed");
            return INVALID_OPERATION;
        }

        LOG_INFO(LOG_TAG, "decode size : %llu", m_decSize);
        m_decBuf       = std::make_shared<AudioBuffer>(m_decSize);
        off64_t offset = 0;
        for (auto &buf : m_decBufVec) {
            m_decBuf->setData(offset, buf->size(), buf->data());
            offset += buf->size();
        }

        m_decBufVec.clear();
        size_t bytesPreMs = m_spec.sampleRate * m_spec.numChannel * m_spec.bytesPerSample / 1000;
        m_spec.durationMs = m_decSize / bytesPreMs;
    }
    return NO_ERROR;
}
