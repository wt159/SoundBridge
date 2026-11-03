#include "AudioStreamDecoder.h"
#include "FLACDecode.h"
#include "LogWrapper.h"
#include "VorbisDecode.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#define LOG_TAG "AudioStreamDecoder"

namespace {
static bool shouldTrustExtractorPcmFormat(AudioCodecID codecId)
{
    return codecId == AUDIO_CODEC_ID_NONE || codecId == AUDIO_CODEC_ID_FLAC
        || codecId == AUDIO_CODEC_ID_VORBIS;
}
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
    , m_errorCode(sdk::ErrorCode::Ok)
    , m_errorMutex()
    , m_errorDetail()
    , m_stopRequest(false)
    , m_seekRequestMs(-1)
    , m_seekDiscardBytes(0)
    , m_processedBytes(0)
    , m_consumedBytes(0)
    , m_bytesPerMs(0)
    , m_abortDecode(false)
    , m_decodedFrames(0)
    , m_decodedRate(0)
    , m_pcmFrameBytes(0)
    , m_seekBaseMs(0)
    , m_skippedFrames(0)
    , m_pendingFrameSkip(false)
    , m_frameSkipWithDataAfter(false)
    , m_md5Mismatch(false)
    , m_thread()
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

sdk_utils::status_t AudioStreamDecoder::start(ExtractorHelper *extractor, bool startThread)
{
    stop();
    m_errorCode.store(sdk::ErrorCode::Ok);
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_errorDetail.clear();
    }
    m_extractor = extractor;
    if (m_ring == nullptr || m_extractor == nullptr) {
        setError(sdk::ErrorCode::DecodeInitFailed, "decoder start with null ring or extractor");
        return sdk_utils::INVALID_OPERATION;
    }

    m_srcSpec    = m_extractor->getAudioSpec();
    m_codecID    = m_extractor->getAudioCodecID();
    m_durationMs = m_srcSpec.durationMs;

    if (!shouldTrustExtractorPcmFormat(m_codecID)) {
        m_srcSpec.format         = AudioFormatUnknown;
        m_srcSpec.bytesPerSample = 0;
    }

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
            auto r = m_lazyResample->Value();
            if (r == nullptr) {
                setError(sdk::ErrorCode::DecodeInitFailed, "resample init failed");
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
    m_decodedFrames = 0;
    m_seekBaseMs    = 0;
    m_skippedFrames = 0;
    m_pendingFrameSkip      = false;
    m_frameSkipWithDataAfter = false;
    m_md5Mismatch           = false;
    // 解出量的计量：源格式每帧字节数与采样率。format 未知的容器（mp3 等）在首帧回调里回填。
    m_decodedRate   = static_cast<uint32_t>(m_srcSpec.sampleRate);
    m_pcmFrameBytes = static_cast<size_t>(m_srcSpec.numChannel)
        * static_cast<size_t>(m_srcSpec.bytesPerSample);
    m_ring->reset();
    if (startThread) {
        m_state.store(StreamDecoderState::DECODING);
        m_thread = std::thread(&AudioStreamDecoder::threadFunc, this);
    } else {
        m_state.store(StreamDecoderState::IDLE);
    }
    return sdk_utils::OK;
}

sdk_utils::status_t AudioStreamDecoder::start(ExtractorHelper *extractor)
{
    return start(extractor, true);
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

sdk::ErrorCode AudioStreamDecoder::errorCode() const
{
    return m_errorCode.load();
}

std::string AudioStreamDecoder::errorDetail() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_errorDetail;
}

void AudioStreamDecoder::finishAtEndOfStream()
{
    // 规格 T4 / 条目 23：EOS 汇总处比「解出多少」与「标称多长」。
    // 修好前 flac 截到 60% 一路走到 EOS，用户看到进度条走到 177 秒就结束、界面还写着
    // 297 秒——「播到坏处」必须说出来，只有 P0（切在音频之后）才静默。
    //
    // 两道判据，都是「事实上报 + 单一阈值」：
    //   1. 提取器能确定短读（wav/aiff 声明长度 > 文件剩余）→ 与百分比无关地报；
    //   2. 否则解出时长短于标称 5 个百分点以上才报。5pp 的来源是正常文件的量化误差
    //      （编码器 padding、seek 偏移、asf 的 +0.20%）都在它以内。
    // D 类（mp3/aac/mkv）不受影响：它们的标称时长跟着文件一起缩水，measured ≈ declared。

    // 规格 F1 / F2：坏帧的汇总是「跳过继续」的最后一步，优先级高于截断。
    // 只有「跳帧之后还有数据」才算坏帧——失败发生在数据末尾属于截断（4.1）。
    if (m_frameSkipWithDataAfter) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "skipped %llu undecodable frame(s)",
                      static_cast<unsigned long long>(m_skippedFrames));
        setError(sdk::ErrorCode::DataCorrupted, detail);
        return;
    }

    if (m_extractor != nullptr && m_extractor->isDataTruncated()) {
        setError(sdk::ErrorCode::DataTruncated, "declared payload length exceeds the file size");
        return;
    }

    const bool haveNominal = m_durationMs > 0 && m_decodedRate > 0;
    const uint64_t measuredMs = haveNominal
        ? m_seekBaseMs + m_decodedFrames * 1000 / static_cast<uint64_t>(m_decodedRate)
        : 0;
    // 正常 EOS 时短于标称超过 5 个百分点（T4）
    const bool shortOfNominal = haveNominal && measuredMs < m_durationMs
        && (m_durationMs - measuredMs) * 20 > m_durationMs;

    // flac 的整段 MD5：截断会让 MD5 同样不符，所以「解不到标称长度」时按截断处理，
    // 只有「解到了标称长度却对不上」才是流内坏帧（4.1：坏帧之后还有数据）。
    if (m_md5Mismatch && !shortOfNominal) {
        setError(sdk::ErrorCode::DataCorrupted, "flac stream md5 mismatch");
        return;
    }

    if (shortOfNominal) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "decoded %llu ms of nominal %llu ms",
                      static_cast<unsigned long long>(measuredMs),
                      static_cast<unsigned long long>(m_durationMs));
        setError(sdk::ErrorCode::DataTruncated, detail);
        return;
    }

    m_state.store(StreamDecoderState::EOS);
}

void AudioStreamDecoder::noteSkippedFrame(uint64_t count)
{
    m_skippedFrames += count;
    m_pendingFrameSkip = true;
}

void AudioStreamDecoder::onAudioDecodeSkip()
{
    noteSkippedFrame();
}

void AudioStreamDecoder::setError(sdk::ErrorCode code, const char *detail)
{
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_errorDetail = (detail != nullptr) ? detail : "";
    }
    m_errorCode.store(code);
    m_state.store(StreamDecoderState::ERROR);
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
    const size_t minSpace = calculateMinWriteSpace(m_codecID);

    if (m_ring->availableWrite() < minSpace) {
        return false;
    }

    return true;
}

void AudioStreamDecoder::threadFunc()
{
    if (m_ring == nullptr || m_extractor == nullptr) {
        setError(sdk::ErrorCode::DecodeInitFailed, "decoder thread without ring or extractor");
        return;
    }

    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flac(this);
        flac.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            // 从文件起点喂，而不是 getAudioDataOffset()（元数据之后）。
            // libFLAC 只在读到 STREAMINFO 之后才会做整段 MD5 校验（没有 STREAMINFO 时它
            // 直接 do_md5_checking=false），所以只喂纯帧数据等于把 #10 的检测手段关掉。
            if (!flac.initFromDataSource(ds, 0, m_extractor->getDataSize())) {
                setError(sdk::ErrorCode::DecodeInitFailed, "flac stream init failed");
                return;
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                setError(sdk::ErrorCode::DataCorrupted, "extractor metadata unavailable");
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
                m_decodedFrames    = 0;
                m_seekBaseMs       = static_cast<uint64_t>(seekMs);
                FLAC__uint64 targetSample
                    = static_cast<FLAC__uint64>(seekMs) * m_srcSpec.sampleRate / 1000;
                if (!flac.seekToSample(targetSample)) {
                    setError(sdk::ErrorCode::DataCorrupted, "flac seek failed");
                    break;
                }
                m_abortDecode.store(false);
                m_state.store(StreamDecoderState::DECODING);
            }

            int ret = flac.processOne();
            if (ret == 0) {
                // 帧级信号（error_callback 的 LOST_SYNC）分不清「跳了帧」还是「解了垃圾」，
                // 整段校验只能靠 STREAMINFO 的 MD5，而 libFLAC 只在 finish() 里比它。
                m_md5Mismatch = flac.md5MismatchAtEndOfStream();
                finishAtEndOfStream();
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
                setError(sdk::ErrorCode::DataCorrupted, "flac decode failed");
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
                setError(sdk::ErrorCode::DecodeInitFailed, "vorbis stream init failed");
                return;
            }
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                setError(sdk::ErrorCode::DataCorrupted, "extractor metadata unavailable");
                return;
            }
            if (!vorbis.initVF(extPtr->data(), extPtr->size())) {
                setError(sdk::ErrorCode::DecodeInitFailed, "vorbis stream init failed");
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
                m_decodedFrames    = 0;
                m_seekBaseMs       = static_cast<uint64_t>(seekMs);
                if (!vorbis.seekToMs(static_cast<uint64_t>(seekMs))) {
                    setError(sdk::ErrorCode::DataCorrupted, "vorbis seek failed");
                    break;
                }
                m_abortDecode.store(false);
                m_state.store(StreamDecoderState::DECODING);
            }

            int ret = vorbis.decodeOne();
            if (ret == 0) {
                finishAtEndOfStream();
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
                setError(sdk::ErrorCode::DataCorrupted, "vorbis decode failed");
                break;
            }
            if (ret == kVorbisHoleSkipped) {
                // 坏页：记账后继续读下一页（规格 4.3 / 条目 12），EOS 处汇总成一条错误。
                noteSkippedFrame();
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
            m_decodedFrames = 0;
            m_seekBaseMs    = static_cast<uint64_t>(seekMs);
            m_seekRequestMs.store(-1);
            m_abortDecode.store(false);
            m_state.store(StreamDecoderState::DECODING);

            RunDecodeResult ret = runDecode();
            if (ret == RunDecodeResult::Error) {
                setError(sdk::ErrorCode::DataCorrupted, "decode failed");
                break;
            }
            if (m_stopRequest.load()) {
                break;
            }
            if (m_seekRequestMs.load() >= 0) {
                continue;
            }
            finishAtEndOfStream();
            break;
        }

        if (m_stopRequest.load()) {
            break;
        }

        // 在解码前添加背压检查
        const size_t minSpace = calculateMinWriteSpace(m_codecID);

        if (m_ring->availableWrite() < minSpace) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        RunDecodeResult ret = runDecode();
        if (ret == RunDecodeResult::Error) {
            setError(sdk::ErrorCode::DataCorrupted, "decode failed");
        } else if (ret == RunDecodeResult::Abort && m_seekRequestMs.load() >= 0) {
            continue;
        } else if (!m_stopRequest.load()) {
            finishAtEndOfStream();
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
        m_srcSpec.sampleRate     = out.spec.sampleRate;
        m_srcSpec.numChannel     = out.spec.numChannel;
        m_srcSpec.format         = out.spec.format;
        m_srcSpec.bytesPerSample = out.spec.bytesPerSample;
        m_srcSpec.bitsPerSample  = out.spec.bitsPerSample;
        m_bytesPerMs             = static_cast<uint64_t>(m_srcSpec.sampleRate)
            * static_cast<uint64_t>(m_srcSpec.numChannel)
            * static_cast<uint64_t>(m_srcSpec.bytesPerSample) / 1000;

        if (m_srcSpec == m_devSpec) {
            m_lazyResample = Optional<Lazy<std::shared_ptr<AudioResample>>>();
        } else {
            m_lazyResample->Value(); // 触发工厂函数，此时 m_srcSpec 已更新为真实解码格式
        }
    }

    size_t frameBytes = static_cast<size_t>(out.spec.samples)
        * static_cast<size_t>(out.spec.numChannel) * static_cast<size_t>(out.spec.bytesPerSample);
    if (frameBytes == 0 || out.lineData == nullptr) {
        return;
    }

    // 首帧的真实规格：解码器说了算，EOS 汇总的换算基准跟着它走（mp3 等容器头里没有）
    if (out.spec.sampleRate > 0 && out.spec.numChannel > 0 && out.spec.bytesPerSample > 0) {
        m_decodedRate   = static_cast<uint32_t>(out.spec.sampleRate);
        m_pcmFrameBytes = static_cast<size_t>(out.spec.numChannel)
            * static_cast<size_t>(out.spec.bytesPerSample);
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

    // 跳帧之后又解出了数据 → 这一次是「坏帧」而不是「截断」（规格 4.1）。
    if (m_pendingFrameSkip) {
        m_pendingFrameSkip      = false;
        m_frameSkipWithDataAfter = true;
    }
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

    if (m_pcmFrameBytes > 0) {
        m_decodedFrames += static_cast<uint64_t>(writeLen / m_pcmFrameBytes);
    }

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

AudioStreamDecoder::RunDecodeResult AudioStreamDecoder::runDecode()
{
    if (m_extractor == nullptr) {
        return RunDecodeResult::Error;
    }

    if (m_codecID == AUDIO_CODEC_ID_NONE) {
        AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
        if (extPtr == nullptr || extPtr->size() == 0) {
            LOGE("extractor getMetaData failed");
            return RunDecodeResult::Error;
        }
        if (!writeDecodedBytes(extPtr->data(), extPtr->size())) {
            return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Error;
        }
        return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Completed;
    }

    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flacDecode(this);
        flacDecode.setAbortFlag(&m_abortDecode);
        DataSourceBase *ds = m_extractor->getDataSource();
        int ret            = -1;
        if (ds != nullptr) {
            flacDecode.initFromDataSource(ds, 0, m_extractor->getDataSize());
            ret = flacDecode.process_until_end_of_stream() ? 0 : -1;
        } else {
            AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
            if (extPtr == nullptr || extPtr->size() == 0) {
                LOGE("extractor getMetaData failed");
                return RunDecodeResult::Error;
            }
            ret = flacDecode.decode(extPtr);
        }
        if (ret < 0) {
            return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Error;
        }
        return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Completed;
    }

    if (m_codecID == AUDIO_CODEC_ID_VORBIS) {
        VorbisDecode vorbisDecode(this);
        vorbisDecode.setAbortFlag(&m_abortDecode);
        int ret            = -1;
        DataSourceBase *ds = m_extractor->getDataSource();
        if (ds != nullptr) {
            if (!vorbisDecode.initVFFromSource(ds, m_extractor->getDataSize())) {
                return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Error;
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
                return RunDecodeResult::Error;
            }
            ret = vorbisDecode.decode(extPtr);
        }
        if (ret < 0) {
            return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Error;
        }
        return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Completed;
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
        return RunDecodeResult::Error;
    }
    std::vector<AudioBuffer::AudioBufferPtr> packetizedMetaData
        = m_extractor->getPacketizedMetaData();
    int ret = 0;
    if (!packetizedMetaData.empty()) {
        ret = decode.decodePackets(packetizedMetaData);
    } else {
        AudioBuffer::AudioBufferPtr extPtr = m_extractor->getMetaData();
        if (extPtr == nullptr || extPtr->size() == 0) {
            LOGE("extractor getMetaData failed");
            return RunDecodeResult::Error;
        }
        ret = decode.decode(extPtr->data(), extPtr->size());
    }
    // 跳过的包已经在发生顺序上经 onAudioDecodeSkip() 记过账，上报留给 EOS 汇总
    // （规格 F1：一次坏文件只挂一条错误）；这里只区分「放弃」与「正常跑完」。
    if (ret < 0) {
        return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Error;
    }
    return m_abortDecode.load() ? RunDecodeResult::Abort : RunDecodeResult::Completed;
}
