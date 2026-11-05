#include "AudioDecodeProcess.h"
#include "FLACDecode.h"
#include "LogWrapper.h"
#include "VorbisDecode.h"

#define LOG_TAG "AudioDecodeProcess"

using namespace sdk_utils;

AudioDecodeProcess::AudioDecodeProcess(ExtractorHelper *extractor, PayloadSupply supply)
    : m_extractor(extractor)
    , m_decode(nullptr)
    , m_decSize(0)
    , m_supply(supply)
    , m_payloadPacketCount(0)
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
    for (size_t i = 0; i < out.spec.samples; i++) {
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

void AudioDecodeProcess::mergeDecodeBuffer(AudioBufferPtr &outBuf,
                                           std::vector<AudioBuffer::AudioBufferPtr> &inBufVec)
{
    size_t size = 0;
    for (auto &buf : inBufVec) {
        size += buf->size();
    }
    LOGI("mergeDecodeBuffer size: %ld", size);
    outBuf         = std::make_shared<AudioBuffer>(size);
    off64_t offset = 0;
    for (auto &buf : inBufVec) {
        outBuf->setData(offset, buf->size(), buf->data());
        offset += buf->size();
    }
}

status_t AudioDecodeProcess::init()
{
    if (m_codecID == AUDIO_CODEC_ID_NONE) {
        AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
        if (extPtr == nullptr) {
            LOGE("getMetaData failed");
            return INVALID_OPERATION;
        }
        LOG_INFO(LOG_TAG, "extractor buffer size(): %ld", extPtr->size());
        LOG_ERROR(LOG_TAG, "m_codecID is AUDIO_CODEC_ID_NONE, not need decode");
        m_decBuf  = extPtr;
        m_decSize = m_decBuf->size();
    } else if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flacDecode(this);
        DataSourceBase *dataSource = m_extractor->getDataSource();
        if (dataSource != nullptr) {
            // 与流式路径一致：从文件起点喂，libFLAC 需要 STREAMINFO 才能做整段 MD5 校验。
            if (!flacDecode.initFromDataSource(dataSource, 0, m_extractor->getDataSize())) {
                LOG_ERROR(LOG_TAG, "initFromDataSource failed");
                return INVALID_OPERATION;
            }
            while (true) {
                int ret = flacDecode.processOne();
                if (ret == 0) {
                    break;
                }
                if (ret < 0) {
                    LOG_ERROR(LOG_TAG, "decode failed");
                    return INVALID_OPERATION;
                }
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr) {
                LOGE("getMetaData failed");
                return INVALID_OPERATION;
            }
            LOG_INFO(LOG_TAG, "extractor buffer size(): %ld", extPtr->size());
            int ret = flacDecode.decode(extPtr);
            if (ret < 0) {
                LOG_ERROR(LOG_TAG, "decode failed");
                return INVALID_OPERATION;
            }
        }
        mergeDecodeBuffer(m_decBuf, m_decBufVec);
        m_decBufVec.clear();
        size_t bytesPreMs = m_spec.sampleRate * m_spec.numChannel * m_spec.bytesPerSample;
        if (bytesPreMs == 0) {
            // #21：一帧都没解出来时 bytesPerSample 可能仍是 0（mp3/ogg 的 spec 允许未知），
            // 这里不能除，否则整除崩溃；时长留 0，语义交给 #15 定案。
            LOG_WARNING(LOG_TAG, "bytesPerMs is 0 (no decoded frame), durationMs left 0");
            m_spec.durationMs = 0;
        } else {
            m_spec.durationMs = m_decSize * 1000 / bytesPreMs;
        }
    } else if (m_codecID == AUDIO_CODEC_ID_VORBIS) {
        VorbisDecode vorbisDecode(this);
        DataSourceBase *dataSource = m_extractor->getDataSource();
        if (dataSource != nullptr) {
            if (!vorbisDecode.initVFFromSource(dataSource, m_extractor->getDataSize())) {
                LOG_ERROR(LOG_TAG, "initVFFromSource failed");
                return INVALID_OPERATION;
            }
            while (true) {
                int ret = vorbisDecode.decodeOne();
                if (ret == 0) {
                    break;
                }
                if (ret < 0) {
                    LOG_ERROR(LOG_TAG, "decode failed");
                    return INVALID_OPERATION;
                }
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr) {
                LOGE("getMetaData failed");
                return INVALID_OPERATION;
            }
            LOG_INFO(LOG_TAG, "extractor buffer size(): %ld", extPtr->size());
            int ret = vorbisDecode.decode(extPtr);
            if (ret < 0) {
                LOG_ERROR(LOG_TAG, "decode failed");
                return INVALID_OPERATION;
            }
        }
        mergeDecodeBuffer(m_decBuf, m_decBufVec);
        m_decBufVec.clear();
        size_t bytesPreMs = m_spec.sampleRate * m_spec.numChannel * m_spec.bytesPerSample;
        if (bytesPreMs == 0) {
            // #21：一帧都没解出来时 bytesPerSample 可能仍是 0（mp3/ogg 的 spec 允许未知），
            // 这里不能除，否则整除崩溃；时长留 0，语义交给 #15 定案。
            LOG_WARNING(LOG_TAG, "bytesPerMs is 0 (no decoded frame), durationMs left 0");
            m_spec.durationMs = 0;
        } else {
            m_spec.durationMs = m_decSize * 1000 / bytesPreMs;
        }
    } else {
        AudioCodecConfig config;
        config.sampleRate    = m_spec.sampleRate;
        config.channels      = m_spec.numChannel;
        config.bitsPerSample = m_spec.bitsPerSample;
        config.bitRate       = m_extractor->getBitRate();
        config.blockAlign    = m_extractor->getBlockAlign();
        config.extraData     = m_extractor->getCodecExtraData();
        m_decode             = std::make_shared<AudioDecode>(m_codecID, this, config);
        if (m_decode == nullptr || m_decode->initCheck() != OK) {
            LOG_ERROR(LOG_TAG, "new AudioDecode failed or initCheck failed, %p", m_decode.get());
            return INVALID_OPERATION;
        }
        std::vector<AudioBuffer::AudioBufferPtr> packetizedMetaData;
        // 供给形式只影响「负载从哪来」（#57 expand 阶段）：解码本身仍是同一套。
        // 默认路径逐字保持旧行为；单包迭代路径用 PacketCursor 逐包拼出同一段负载。
        if (m_supply == PayloadSupply::PacketIteration) {
            PacketCursor cursor(m_extractor);
            AudioBuffer::AudioBufferPtr packet;
            while (cursor.nextPacket(packet)) {
                packetizedMetaData.push_back(packet);
            }
        } else {
            packetizedMetaData = m_extractor->getPacketizedMetaData();
        }
        m_payloadPacketCount = packetizedMetaData.size();
        int ret              = 0;
        if (!packetizedMetaData.empty()) {
            ret = m_decode->decodePackets(packetizedMetaData);
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr) {
                LOGE("getMetaData failed");
                return INVALID_OPERATION;
            }
            LOG_INFO(LOG_TAG, "extractor buffer size(): %ld", extPtr->size());
            ret = m_decode->decode(extPtr->data(), extPtr->size());
        }
        if (ret < 0) {
            LOG_ERROR(LOG_TAG, "decode failed");
            return INVALID_OPERATION;
        }

        mergeDecodeBuffer(m_decBuf, m_decBufVec);

        m_decBufVec.clear();
        size_t bytesPreMs = m_spec.sampleRate * m_spec.numChannel * m_spec.bytesPerSample;
        if (bytesPreMs == 0) {
            // #21：一帧都没解出来时 bytesPerSample 可能仍是 0（mp3/ogg 的 spec 允许未知），
            // 这里不能除，否则整除崩溃；时长留 0，语义交给 #15 定案。
            LOG_WARNING(LOG_TAG, "bytesPerMs is 0 (no decoded frame), durationMs left 0");
            m_spec.durationMs = 0;
        } else {
            m_spec.durationMs = m_decSize * 1000 / bytesPreMs;
        }
    }
    return NO_ERROR;
}
