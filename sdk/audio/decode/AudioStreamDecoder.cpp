#include "AudioStreamDecoder.h"
#include "FLACDecode.h"
#include "LogWrapper.h"
#include "VorbisDecode.h"
#include <algorithm>
#include <chrono>
#include <cstring>

#define LOG_TAG "AudioStreamDecoder"

namespace {
static const int kDecodeOk    = 0;
static const int kDecodeAbort = 1;
static const int kDecodeError = -1;
}

AudioStreamDecoder::AudioStreamDecoder(AudioRingBuffer *ring, const AudioSpec &devSpec)
    : m_ring(ring)
    , m_devSpec(devSpec)
    , m_srcSpec()
    , m_extractor(nullptr)
    , m_codecID(AUDIO_CODEC_ID_NONE)
    , m_durationMs(0)
    , m_lazyResample()
    , m_interleaveBuf()
    , m_resampleBuf()
    , m_state(StreamDecoderState::IDLE)
    , m_stopRequest(false)
    , m_seekRequestMs(-1)
    , m_seekDiscardBytes(0)
    , m_processedBytes(0)
    , m_consumedBytes(0)
    , m_bytesPerMs(0)
    , m_abortDecode(false)
    , m_thread()
    , m_adapter()
    , m_options()
{
}

size_t AudioStreamDecoder::calculateMinWriteSpace(AudioCodecID codecId) const
{
    switch (codecId) {
    case AUDIO_CODEC_ID_FLAC:
        return 32 * 1024; // 32KB - FLAC frame typically 16-64KB
    case AUDIO_CODEC_ID_VORBIS:
        return 16 * 1024; // 16KB - Vorbis typical ov_read is 8KB
    case AUDIO_CODEC_ID_AAC:
    case AUDIO_CODEC_ID_MP3:
        return 8 * 1024; // 8KB - FFmpeg conservative
    default:
        return 4 * 1024; // 4KB default
    }
}

AudioStreamDecoder::~AudioStreamDecoder()
{
    stop();
}

sdk_utils::status_t AudioStreamDecoder::start(ExtractorHelper *extractor)
{
    stop();
    m_extractor = extractor;
    if (m_ring == nullptr || m_extractor == nullptr) {
        m_state.store(StreamDecoderState::ERROR);
        return sdk_utils::INVALID_OPERATION;
    }

    m_srcSpec    = m_extractor->getAudioSpec();
    m_codecID    = m_extractor->getAudioCodecID();
    m_durationMs = m_srcSpec.durationMs;

    m_bytesPerMs = static_cast<uint64_t>(m_srcSpec.sampleRate)
        * static_cast<uint64_t>(m_srcSpec.numChannel)
        * static_cast<uint64_t>(m_srcSpec.bytesPerSample) / 1000;

    m_lazyResample = Optional<Lazy<std::shared_ptr<AudioResample>>>();
    if (!(m_srcSpec == m_devSpec)) {
        auto factory = [this]() -> std::shared_ptr<AudioResample> {
            AudioSpec inSpec  = m_srcSpec;
            AudioSpec outSpec = m_devSpec;
            inSpec.samples    = 1024;
            auto r            = std::make_shared<AudioResample>(inSpec, outSpec);
            if (!r || r->initCheck() != sdk_utils::OK) {
                LOGE("lazy resample init failed");
                return nullptr;
            }
            LOGI("lazy resample init ok, format=%d", m_srcSpec.format);
            return r;
        };
        m_lazyResample.emplace(factory);
        if (m_srcSpec.format != AudioFormatUnknown) {
            // format 已知时立即初始化，否则等首帧回调
            auto r = m_lazyResample->Value();
            if (r == nullptr) {
                m_state.store(StreamDecoderState::ERROR);
                return sdk_utils::INVALID_OPERATION;
            }
        } else {
            LOGI("srcSpec format unknown, defer resample init to first decode callback");
        }
    }

    m_stopRequest.store(false);
    m_abortDecode.store(false);
    m_seekRequestMs.store(-1);
    m_seekDiscardBytes = 0;
    m_processedBytes   = 0;
    m_consumedBytes.store(0);
    m_ring->reset();
    m_state.store(StreamDecoderState::DECODING);
    m_thread = std::thread(&AudioStreamDecoder::threadFunc, this);
    return sdk_utils::OK;
}

void AudioStreamDecoder::stop()
{
    m_stopRequest.store(true);
    m_abortDecode.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_stopRequest.store(false);
    m_abortDecode.store(false);
    m_state.store(StreamDecoderState::IDLE);
}

void AudioStreamDecoder::seekToMs(uint64_t targetMs)
{
    if (m_state.load() == StreamDecoderState::IDLE || m_state.load() == StreamDecoderState::ERROR) {
        return;
    }
    m_seekRequestMs.store(static_cast<int64_t>(targetMs));
    m_abortDecode.store(true);
}

StreamDecoderState AudioStreamDecoder::state() const
{
    return m_state.load();
}

uint64_t AudioStreamDecoder::positionMs() const
{
    if (m_bytesPerMs == 0) {
        return 0;
    }
    return m_consumedBytes.load(std::memory_order_relaxed) / m_bytesPerMs;
}

uint64_t AudioStreamDecoder::durationMs() const
{
    return m_durationMs;
}

bool AudioStreamDecoder::canDecode() const
{
    StreamDecoderState currentState = m_state.load();

    // 未启动或已结束
    if (currentState == StreamDecoderState::IDLE || currentState == StreamDecoderState::EOS
        || currentState == StreamDecoderState::ERROR) {
        return false;
    }

    // 停止请求
    if (m_stopRequest.load() || m_abortDecode.load()) {
        return false;
    }

    // ring 无空间
    size_t minSpace
        = m_options.minWriteSpace > 0 ? m_options.minWriteSpace : calculateMinWriteSpace(m_codecID);

    if (m_ring->availableWrite() < minSpace) {
        return false;
    }

    return true;
}

void AudioStreamDecoder::threadFunc()
{
    if (m_ring == nullptr || m_extractor == nullptr) {
        m_state.store(StreamDecoderState::ERROR);
        return;
    }

    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flac(this);
        flac.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            if (!flac.initFromDataSource(ds, m_extractor->getAudioDataOffset(),
                                         m_extractor->getDataSize())) {
                m_state.store(StreamDecoderState::ERROR);
                return;
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                m_state.store(StreamDecoderState::ERROR);
                return;
            }
            flac.setInputBuffer(extPtr);
        }

        while (!m_stopRequest.load()) {
            int64_t seekMs = m_seekRequestMs.exchange(-1);
            if (seekMs >= 0) {
                m_state.store(StreamDecoderState::SEEKING);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                m_ring->reset();
                m_consumedBytes.store(static_cast<uint64_t>(seekMs) * m_bytesPerMs);
                m_processedBytes   = m_consumedBytes.load();
                m_seekDiscardBytes = 0;
                FLAC__uint64 targetSample
                    = static_cast<FLAC__uint64>(seekMs) * m_srcSpec.sampleRate / 1000;
                if (!flac.seekToSample(targetSample)) {
                    m_state.store(StreamDecoderState::ERROR);
                    break;
                }
                m_abortDecode.store(false);
                m_state.store(StreamDecoderState::DECODING);
            }

            int ret = flac.processOne();
            if (ret == 0) {
                m_state.store(StreamDecoderState::EOS);
                break;
            }
            if (ret < 0) {
                if (m_stopRequest.load()) {
                    break;
                }
                if (m_seekRequestMs.load() >= 0) {
                    m_abortDecode.store(false);
                    continue;
                }
                m_state.store(StreamDecoderState::ERROR);
                break;
            }
        }

        if (m_stopRequest.load()) {
            m_state.store(StreamDecoderState::IDLE);
        }
        return;
    }

    if (m_codecID == AUDIO_CODEC_ID_VORBIS) {
        VorbisDecode vorbis(this);
        vorbis.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            if (!vorbis.initVFFromSource(ds, m_extractor->getDataSize())) {
                m_state.store(StreamDecoderState::ERROR);
                return;
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                m_state.store(StreamDecoderState::ERROR);
                return;
            }
            if (!vorbis.initVF(extPtr->data(), extPtr->size())) {
                m_state.store(StreamDecoderState::ERROR);
                return;
            }
        }

        while (!m_stopRequest.load()) {
            int64_t seekMs = m_seekRequestMs.exchange(-1);
            if (seekMs >= 0) {
                m_state.store(StreamDecoderState::SEEKING);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                m_ring->reset();
                m_consumedBytes.store(static_cast<uint64_t>(seekMs) * m_bytesPerMs);
                m_processedBytes   = m_consumedBytes.load();
                m_seekDiscardBytes = 0;
                if (!vorbis.seekToMs(static_cast<uint64_t>(seekMs))) {
                    m_state.store(StreamDecoderState::ERROR);
                    break;
                }
                m_abortDecode.store(false);
                m_state.store(StreamDecoderState::DECODING);
            }

            int ret = vorbis.decodeOne();
            if (ret == 0) {
                m_state.store(StreamDecoderState::EOS);
                break;
            }
            if (ret < 0) {
                if (m_stopRequest.load()) {
                    break;
                }
                if (m_seekRequestMs.load() >= 0) {
                    m_abortDecode.store(false);
                    continue;
                }
                m_state.store(StreamDecoderState::ERROR);
                break;
            }
        }

        if (m_stopRequest.load()) {
            m_state.store(StreamDecoderState::IDLE);
        }
        return;
    }

    for (;;) {
        int64_t seekMs = m_seekRequestMs.load();
        if (seekMs >= 0) {
            m_state.store(StreamDecoderState::SEEKING);
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            m_ring->reset();
            m_consumedBytes.store(0);
            m_processedBytes = 0;
            m_seekDiscardBytes
                = (m_bytesPerMs == 0) ? 0 : static_cast<size_t>(seekMs) * m_bytesPerMs;
            m_seekRequestMs.store(-1);
            m_abortDecode.store(false);
            m_state.store(StreamDecoderState::DECODING);

            int ret = runDecode();
            if (ret == kDecodeError) {
                m_state.store(StreamDecoderState::ERROR);
                break;
            }
            if (m_stopRequest.load()) {
                break;
            }
            if (m_seekRequestMs.load() >= 0) {
                continue;
            }
            m_state.store(StreamDecoderState::EOS);
            break;
        }

        if (m_stopRequest.load()) {
            break;
        }

        int ret = runDecode();
        if (ret == kDecodeError) {
            m_state.store(StreamDecoderState::ERROR);
        } else if (ret == kDecodeAbort && m_seekRequestMs.load() >= 0) {
            continue;
        } else if (!m_stopRequest.load()) {
            m_state.store(StreamDecoderState::EOS);
        }
        break;
    }

    if (m_stopRequest.load()) {
        m_state.store(StreamDecoderState::IDLE);
    }
}

void AudioStreamDecoder::onAudioDecodeCallback(AudioDecodeSpec &out)
{
    if (m_abortDecode.load()) {
        return;
    }

    if (m_stopRequest.load()) {
        m_abortDecode.store(true);
        return;
    }

    // 懒初始化：format 未知时在首帧回调处触发 Lazy 工厂
    if (m_lazyResample.isInit() && !m_lazyResample->IsValueCreated()
        && out.spec.format != AudioFormatUnknown) {
        m_srcSpec.format         = out.spec.format;
        m_srcSpec.bytesPerSample = out.spec.bytesPerSample;
        m_srcSpec.bitsPerSample  = out.spec.bitsPerSample;
        m_lazyResample->Value(); // 触发工厂函数，此时 m_srcSpec.format 已更新
    }

    size_t frameBytes = static_cast<size_t>(out.spec.samples)
        * static_cast<size_t>(out.spec.numChannel) * static_cast<size_t>(out.spec.bytesPerSample);
    if (frameBytes == 0 || out.lineData == nullptr) {
        return;
    }

    m_interleaveBuf.resize(frameBytes);
    size_t offset = 0;
    for (size_t i = 0; i < static_cast<size_t>(out.spec.samples); ++i) {
        for (int ch = 0; ch < out.spec.numChannel; ++ch) {
            std::memcpy(m_interleaveBuf.data() + offset,
                        out.lineData[ch] + static_cast<size_t>(out.spec.bytesPerSample) * i,
                        static_cast<size_t>(out.spec.bytesPerSample));
            offset += static_cast<size_t>(out.spec.bytesPerSample);
        }
    }

    writeDecodedBytes(m_interleaveBuf.data(), frameBytes);
}

bool AudioStreamDecoder::writeDecodedBytes(const char *pcm, size_t pcmSize)
{
    if (pcm == nullptr || pcmSize == 0) {
        return true;
    }

    size_t skipped = 0;
    if (m_seekDiscardBytes > m_processedBytes) {
        size_t remainDiscard = m_seekDiscardBytes - m_processedBytes;
        if (remainDiscard >= pcmSize) {
            m_processedBytes += pcmSize;
            return true;
        }
        skipped           = remainDiscard;
        m_processedBytes += skipped;
    }

    const char *writePtr = pcm + skipped;
    size_t writeLen      = pcmSize - skipped;

    const char *outData = writePtr;
    size_t outLen       = writeLen;
    std::shared_ptr<AudioResample> resamplePtr
        = m_lazyResample.isInit() && m_lazyResample->IsValueCreated() ? m_lazyResample->Value()
                                                                      : nullptr;
    if (resamplePtr != nullptr && writeLen > 0) {
        uint64_t numerator = static_cast<uint64_t>(writeLen)
            * static_cast<uint64_t>(m_devSpec.sampleRate)
            * static_cast<uint64_t>(m_devSpec.numChannel)
            * static_cast<uint64_t>(m_devSpec.bytesPerSample);
        uint64_t denominator = static_cast<uint64_t>(m_srcSpec.sampleRate)
            * static_cast<uint64_t>(m_srcSpec.numChannel)
            * static_cast<uint64_t>(m_srcSpec.bytesPerSample);
        size_t estimate
            = (denominator == 0) ? writeLen : static_cast<size_t>(numerator / denominator + 4096);
        if (estimate < writeLen) {
            estimate = writeLen;
        }
        m_resampleBuf.resize(estimate);
        size_t resampleLen = estimate;
        int ret
            = resamplePtr->resample((void *)writePtr, writeLen, m_resampleBuf.data(), &resampleLen);
        if (ret < 0) {
            return false;
        }
        outData = m_resampleBuf.data();
        outLen  = resampleLen;
    }

    size_t written = 0;
    while (written < outLen) {
        if (m_abortDecode.load() || m_stopRequest.load()) {
            return false;
        }
        size_t avail = m_ring->availableWrite();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        size_t once = std::min(avail, outLen - written);
        size_t got  = m_ring->write(outData + written, once);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        written += got;
    }

    m_processedBytes += (pcmSize - skipped);
    return true;
}

int AudioStreamDecoder::runDecode()
{
    if (m_extractor == nullptr) {
        return kDecodeError;
    }

    if (m_codecID == AUDIO_CODEC_ID_NONE) {
        AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
        if (extPtr == nullptr || extPtr->size() == 0) {
            LOGE("extractor getMetaData failed");
            return kDecodeError;
        }
        if (!writeDecodedBytes(extPtr->data(), extPtr->size())) {
            return m_abortDecode.load() ? kDecodeAbort : kDecodeError;
        }
        return m_abortDecode.load() ? kDecodeAbort : kDecodeOk;
    }

    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flacDecode(this);
        flacDecode.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        int ret            = -1;
        if (ds != nullptr) {
            flacDecode.initFromDataSource(ds, m_extractor->getAudioDataOffset(),
                                          m_extractor->getDataSize());
            ret = flacDecode.process_until_end_of_stream() ? 0 : -1;
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                return kDecodeError;
            }
            ret = flacDecode.decode(extPtr);
        }
        if (ret < 0) {
            return m_abortDecode.load() ? kDecodeAbort : kDecodeError;
        }
        return m_abortDecode.load() ? kDecodeAbort : kDecodeOk;
    }

    if (m_codecID == AUDIO_CODEC_ID_VORBIS) {
        VorbisDecode vorbisDecode(this);
        vorbisDecode.setAbortFlag(&m_abortDecode);
        int ret            = -1;
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            if (!vorbisDecode.initVFFromSource(ds, m_extractor->getDataSize())) {
                return m_abortDecode.load() ? kDecodeAbort : kDecodeError;
            }
            while (true) {
                ret = vorbisDecode.decodeOne();
                if (ret == 0) {
                    break;
                }
                if (ret < 0) {
                    break;
                }
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                return kDecodeError;
            }
            ret = vorbisDecode.decode(extPtr);
        }
        if (ret < 0) {
            return m_abortDecode.load() ? kDecodeAbort : kDecodeError;
        }
        return m_abortDecode.load() ? kDecodeAbort : kDecodeOk;
    }

    AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
    if (extPtr == nullptr || extPtr->size() == 0) {
        LOGE("extractor getMetaData failed");
        return kDecodeError;
    }

    AudioCodecConfig config;
    config.sampleRate    = m_srcSpec.sampleRate;
    config.channels      = m_srcSpec.numChannel;
    config.bitsPerSample = m_srcSpec.bitsPerSample;
    config.bitRate       = m_extractor->getBitRate();
    config.blockAlign    = m_extractor->getBlockAlign();
    config.extraData     = m_extractor->getCodecExtraData();

    AudioDecode decode(m_codecID, this, config);
    if (decode.initCheck() != sdk_utils::OK) {
        LOGE("AudioDecode init failed, codec=%#x", m_codecID);
        return kDecodeError;
    }
    int ret = decode.decode(extPtr->data(), extPtr->size());
    if (ret < 0) {
        return m_abortDecode.load() ? kDecodeAbort : kDecodeError;
    }
    return m_abortDecode.load() ? kDecodeAbort : kDecodeOk;
}
