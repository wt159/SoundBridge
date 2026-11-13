#include "AudioStreamDecoder.h"
#include "FLACDecode.h"
#include "LogWrapper.h"
#include "VorbisDecode.h"
// P3 单一入口的负返回分类（#151 §6.3）：AVERROR_EXIT / AVERROR_EOF /
// AVERROR_INVALIDDATA 逐字来自 liberror.h；诊断旁路的 int 值即 av_read_frame 返回值。
extern "C" {
#include "libavutil/avutil.h"
}
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
    , m_firstDecodeFrame(true)
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
    , m_segmentBaseBytes(0)
    , m_pendingSeekBaseMs(0)
    , m_bytesPerMs(0)
    , m_abortDecode(false)
    , m_abortSource(nullptr)
    , m_reconnectAttempts(0)
    , m_decodedFrames(0)
    , m_decodedRate(0)
    , m_pcmFrameBytes(0)
    , m_seekBaseMs(0)
    , m_skippedFrames(0)
    , m_pendingFrameSkip(false)
    , m_frameSkipWithDataAfter(false)
    , m_md5Mismatch(false)
    , m_peakHeldBytes(0)
    , m_silenceFills(0)
    , m_startToFirstPcmUs(0)
    , m_initScanUs(0)
    , m_packetHeldBytes(0)
    , m_packetsPulled(0)
    , m_packetsPulledAtFirstPcm(0)
    , m_startClock()
    , m_firstPcmNoted(false)
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

std::shared_ptr<AudioResample> AudioStreamDecoder::createResampler()
{
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
}

bool AudioStreamDecoder::deviceSpecSupportsResample() const
{
    return m_devSpec.sampleRate > 0 && m_devSpec.numChannel > 0 && m_devSpec.bytesPerSample > 0
        && m_devSpec.format != AudioFormatUnknown;
}

sdk_utils::status_t AudioStreamDecoder::start(ExtractorHelper *extractor, bool startThread)
{
    stop();
    // 观测点从这个时刻起算（= start() 的入口）。放在 stop() 之后，
    // 免得把上一次任务的 join 时间算进首播延迟。
    m_startClock = std::chrono::steady_clock::now();
    m_peakHeldBytes.store(0);
    m_silenceFills.store(0);
    m_startToFirstPcmUs.store(0);
    m_initScanUs.store(0);
    m_packetHeldBytes = 0;
    m_packetsPulled.store(0);
    m_packetsPulledAtFirstPcm.store(0);
    m_reconnectAttempts.store(0);
    m_firstPcmNoted = false;

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
    // P1：把唯一取消信号注入数据源层。start() 首行已 stop()，旧的绑定已解。
    DataSourceBase *ds = m_extractor->getDataSource();
    if (ds != nullptr) {
        ds->setAbortFlag(&m_abortDecode);
        m_abortSource = ds;
    }
    // 观测点：提取器 init（只读扫描）耗时由工厂量好，这里抄进本任务的仪表。
    m_initScanUs.store(m_extractor->initScanMicros());
    // 分配记账的第一个采样点：init 完成后提取器已经持有多少字节（增量路径下是 O(1) 摘要 + 解复用缓冲）。
    // 之后的采样点挂在包的 alloc/free 事件上（refreshHeldPeak）——增量供给下持有量会先增后减，
    // 只在 start / 首帧 / EOS 采样会把峰值低估。
    refreshHeldPeak();

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
    m_firstDecodeFrame = true;
    // 规格 6.2 S3：工厂无条件建。声明值恰好等于 device spec 时也要建——首帧的实测值
    // 可能还要覆盖它（修好前只在 `m_srcSpec != m_devSpec` 时建，那门一关首帧就永远覆盖不上）。
    // 设备规格没有 PCM 格式时不建：没有重采样目标格式，建了也只会 init 失败。
    if (!deviceSpecSupportsResample()) {
        LOGW("device spec has no pcm format, resampling disabled");
    } else {
        m_lazyResample.emplace(
            [this]() -> std::shared_ptr<AudioResample> { return createResampler(); });
        if (!(m_srcSpec == m_devSpec)) {
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
    }

    m_stopRequest.store(false);
    m_abortDecode.store(false);
    m_seekRequestMs.store(-1);
    m_seekDiscardBytes = 0;
    m_processedBytes   = 0;
    // 新任务 = 新段：位置从 0 起算（切歌后不复用旧曲的已消费字节）。
    m_segmentBaseBytes.store(0);
    m_pendingSeekBaseMs = 0;
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
    // 这里刻意**不**清 ring：reset() 只能由解码线程发起（规格 4.4），
    // 新段的清零由 threadFunc() 的第一件事完成。生产链每次播放任务都用新 ring，
    // 所以控制线程返回时消费端不可能从这颗 ring 里读到上一首的 PCM。
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
    // P1：解码线程已退出，解绑裸指针，避免 decoder 析构后数据源持有悬垂 flag。
    if (m_abortSource != nullptr) {
        m_abortSource->setAbortFlag(nullptr);
        m_abortSource = nullptr;
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
    // seek 的取消只由目标槽承载（#59 单槽语义）：cancelRequested() 已经把「槽内有值」
    // 视为取消，解码线程的写 ring 循环会中断并回到循环头 exchange 取走目标。
    // 刻意不置 m_abortDecode：它是 stop 的读回调 ABORT 信号（FLACDecode / VorbisDecode
    // 的 m_abortFlag 指向它）。native RA 定位（flac.seekToSample / ov_time_seek）期间
    // 读回调一旦看到它就返回 READ_STATUS_ABORT（#60 的坑）；而「写槽 + 置位」两步发布里
    // 第二步可能晚于解码线程「取槽 → 清位 → 开始定位」，迟到置回会污染整段定位过程，
    // 让 seekToSample() 偶发返回 false（#65 的非确定性根因）。
    // 连续 seek 仍由槽覆盖（后一次覆盖前一次）；stop 仍靠 m_abortDecode 中断读回调。
    m_seekRequestMs.store(static_cast<int64_t>(targetMs));
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

StreamMetrics AudioStreamDecoder::metrics() const
{
    StreamMetrics out;
    out.peakHeldBytes          = m_peakHeldBytes.load();
    out.silenceFills           = m_silenceFills.load();
    out.startToFirstPcmUs      = m_startToFirstPcmUs.load();
    out.initScanUs             = m_initScanUs.load();
    out.packetsPulled          = m_packetsPulled.load();
    out.packetsPulledAtFirstPcm = m_packetsPulledAtFirstPcm.load();
    // 现场重算包边界的 CPU 成本是提取器侧的独立指标（只记录、不设阈值）。
    out.packetBoundaryMicros = m_extractor != nullptr ? m_extractor->packetBoundaryMicros() : 0;
    return out;
}

void AudioStreamDecoder::onPacketAcquired(size_t bytes)
{
    m_packetHeldBytes += bytes;
    refreshHeldPeak();
}

void AudioStreamDecoder::onPacketReleased(size_t bytes)
{
    m_packetHeldBytes = m_packetHeldBytes > bytes ? m_packetHeldBytes - bytes : 0;
    refreshHeldPeak();
}

void AudioStreamDecoder::onPacketPulled(size_t totalPulled)
{
    m_packetsPulled.store(totalPulled);
}

void AudioStreamDecoder::refreshHeldPeak()
{
    size_t held = m_packetHeldBytes;
    if (m_extractor != nullptr) {
        held += m_extractor->heldPayloadBytes();
    }
    // 解码器自己的两块固定缓冲：只随单帧大小变化，不随素材时长增长（判据里的 O(1) 项）。
    held += m_interleaveBuf.capacity() + m_resampleBuf.capacity();
    noteHeldBytes(held);
}

void AudioStreamDecoder::noteSilenceFill()
{
    // 只统计**静默窗口内**的静音填充（规格第 5 节 / 票面 #56）。
    // 窗口的判据来自解码器自己看得见的三类信号：SEEKING、已请求停止、已置取消位。
    // 窗口之外的记账一律丢弃——否则计数器会退化成「消费端调了几次接口」，
    // 而判据要的「稳态窗口静音填充为零」就再也判不出来了。
    const StreamDecoderState current = m_state.load();
    if (current != StreamDecoderState::SEEKING && !m_stopRequest.load() && !m_abortDecode.load()) {
        return;
    }
    m_silenceFills.fetch_add(1, std::memory_order_relaxed);
}

void AudioStreamDecoder::noteHeldBytes(size_t bytes)
{
    size_t peak = m_peakHeldBytes.load(std::memory_order_relaxed);
    while (bytes > peak
           && !m_peakHeldBytes.compare_exchange_weak(peak, bytes, std::memory_order_relaxed)) {
    }
}

void AudioStreamDecoder::finishAtEndOfStream()
{
    // 分配记账的最后一个采样点（其余采样点挂在包的 alloc/free 事件上）。
    refreshHeldPeak();
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
    // #49：seek 目标超出标称时长（可解内容之外）→ 不在 seek 入口拒绝，由 EOS 汇总处
    // 按 m_seekBaseMs 与标称时长比，汇总成既有 DataTruncated。durationMs == 0 时
    // haveNominal=false，不报（规格第 6 节）。
    const bool seekPastEnd = haveNominal && m_seekBaseMs > m_durationMs;

    // flac 的整段 MD5：截断会让 MD5 同样不符，所以「解不到标称长度」时按截断处理，
    // 只有「解到了标称长度却对不上」才是流内坏帧（4.1：坏帧之后还有数据）。
    if (m_md5Mismatch && !shortOfNominal && !seekPastEnd) {
        setError(sdk::ErrorCode::DataCorrupted, "flac stream md5 mismatch");
        return;
    }

    if (shortOfNominal || seekPastEnd) {
        char detail[128];
        if (seekPastEnd && !shortOfNominal) {
            std::snprintf(detail, sizeof(detail), "seek target %llu ms exceeds nominal %llu ms",
                          static_cast<unsigned long long>(m_seekBaseMs),
                          static_cast<unsigned long long>(m_durationMs));
        } else {
            std::snprintf(detail, sizeof(detail), "decoded %llu ms of nominal %llu ms",
                          static_cast<unsigned long long>(measuredMs),
                          static_cast<unsigned long long>(m_durationMs));
        }
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

uint64_t AudioStreamDecoder::positionBytesPerMs() const
{
    if (m_bytesPerMs == 0) {
        return 0;
    }
    // ring 里装的是「写进去的那一档」PCM：重采样生效时是设备格式，否则是源格式。
    // 判断条件与 writeDecodedBytes 建重采样器的条件一致（同一份 spec 比较）。
    if (!deviceSpecSupportsResample() || (m_srcSpec == m_devSpec)) {
        return m_bytesPerMs;
    }
    return static_cast<uint64_t>(m_devSpec.sampleRate)
        * static_cast<uint64_t>(m_devSpec.numChannel)
        * static_cast<uint64_t>(m_devSpec.bytesPerSample) / 1000;
}

void AudioStreamDecoder::applySegmentBase(uint64_t targetMs)
{
    const uint64_t bytesPerMs = positionBytesPerMs();
    if (bytesPerMs == 0) {
        // 首帧之前还不知道 PCM 格式（mp3 / aac 等容器头不声明 bytesPerSample），
        // 算不出字节基准。先寄存目标，等首帧确定字节口径后补算，别把位置钉死成 0。
        m_segmentBaseBytes.store(0);
        m_pendingSeekBaseMs = targetMs;
        return;
    }
    m_segmentBaseBytes.store(targetMs * bytesPerMs);
    m_pendingSeekBaseMs = 0;
}

uint64_t AudioStreamDecoder::positionMs() const
{
    const uint64_t bytesPerMs = positionBytesPerMs();
    if (bytesPerMs == 0 || m_ring == nullptr) {
        return 0;
    }
    // 规格 #55 第 3 节：segmentBaseBytes 是消费端 O(1) 段基准，与 ring 分开存。
    // ring 的 readPos() 只数「本段内被消费端取走的字节」，seek 时会归零；
    // 两者相加才是全程单调的位置，单独用任何一个都会在 seek / 切歌时跳数字。
    const uint64_t consumedThisSegment = m_ring->readPos();
    return (m_segmentBaseBytes.load(std::memory_order_relaxed) + consumedThisSegment)
        / bytesPerMs;
}

uint64_t AudioStreamDecoder::durationMs() const
{
    // 提取器 init 期给出的标称时长，播放期间不变；0 表示未知（规格 #55 第 3 节）。
    // 未知时位置仍按已消费 PCM 报，不从标称时长反推。
    return m_durationMs;
}

bool AudioStreamDecoder::cancelRequested() const
{
    // 取消只由控制线程发布，两个来源：stop 置 m_stopRequest + m_abortDecode；
    // seek 只写 m_seekRequestMs 目标槽（不置 m_abortDecode，见 seekToMs 注释）。
    // 槽里还挂着目标，就说明这次 seek 还没被解码线程处理完，此时任何等待都必须立刻退出，
    // 否则「让位」的那次收尾会把紧接着到达的新目标一起搁死（连续 seek 后一次覆盖前一次）。
    return m_stopRequest.load() || m_abortDecode.load() || m_seekRequestMs.load() >= 0;
}

bool AudioStreamDecoder::canDecode() const
{
    StreamDecoderState currentState = m_state.load();

    // 未启动或已结束
    if (currentState == StreamDecoderState::IDLE || currentState == StreamDecoderState::EOS
        || currentState == StreamDecoderState::ERROR) {
        return false;
    }

    // 停止或 seek 请求
    if (cancelRequested()) {
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

    // 新段起点的清零只在这一处：解码线程是 ring 的唯一生产者，也是唯一能 reset 的人
    // （规格 4.4）。顺序不可反——段基准先归零，再清 ring；消费端此刻处于
    // switching / SEEKING 静默窗口，不读 ring。
    applySegmentBase(0);
    m_ring->reset();

    if (m_codecID == AUDIO_CODEC_ID_FLAC) {
        FLACDecode flac(this);
        flac.setAbortFlag(&m_abortDecode);
        // 供给形式只看「提取器有没有包」：11 种格式全部有包工厂（#66 contract），
        // 逐包 pull（增量）；DataSource 分支只作为 native RA 的备用入口保留。
        // demux 句柄（FLACDecode + 包游标）在这一行之前是不存在的，创建发生在解码线程里、
        // 随本任务退出释放（规格 4.3 所有权清单）。
        DataSourceBase *ds = m_extractor->getDataSource();
        if (m_extractor->packetCount() != 0) {
            if (!flac.initFromPacketFactory(m_extractor, this)) {
                setError(sdk::ErrorCode::DecodeInitFailed, "flac packet supply init failed");
                return;
            }
        } else if (ds != nullptr) {
            // 从文件起点喂，而不是 getAudioDataOffset()（元数据之后）。
            // libFLAC 只在读到 STREAMINFO 之后才会做整段 MD5 校验（没有 STREAMINFO 时它
            // 直接 do_md5_checking=false），所以只喂纯帧数据等于把 #10 的检测手段关掉。
            if (!flac.initFromDataSource(ds, 0, m_extractor->getDataSize())) {
                setError(sdk::ErrorCode::DecodeInitFailed, "flac stream init failed");
                return;
            }
        } else {
            // contract（#66）：整块视图回退口已退场——既没有包工厂也没有 DataSource
            // 时按初始化失败汇总，不再持有整首 payload。
            setError(sdk::ErrorCode::DecodeInitFailed, "flac has no packet supply or data source");
            return;
        }

        while (!m_stopRequest.load()) {
            int64_t seekMs = m_seekRequestMs.exchange(-1);
            if (seekMs >= 0) {
                m_state.store(StreamDecoderState::SEEKING);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                // 段基准先于 ring 归零更新：消费端此刻看到的是「目标 + 旧段已读」的短暂高估，
                // 而不是掉回 0（后者正是段基准必须离开 ring 的原因）。
                applySegmentBase(static_cast<uint64_t>(seekMs));
                m_ring->reset();
                m_processedBytes   = 0;
                m_seekDiscardBytes = 0;
                m_decodedFrames    = 0;
                m_seekBaseMs       = static_cast<uint64_t>(seekMs);
                FLAC__uint64 targetSample
                    = static_cast<FLAC__uint64>(seekMs) * m_srcSpec.sampleRate / 1000;
                // native RA 定位要读流：readFromPackets() 的读回调把 m_abortDecode 当成
                // 停止信号，一旦置位就 READ_STATUS_ABORT（#60 的坑）。seek 现在只写目标槽、
                // 不置该位（见 seekToMs），这里仍显式清一次，保证定位期间读回调不因
                // 上一次 stop 残留的取消位而 ABORT。定位途中到达的新 seek 留在槽里，
                // 由下一轮循环头接住，不影响本次定位（连续 seek 后一次覆盖前一次）。
                m_abortDecode.store(false);
                if (!flac.seekToSample(targetSample)) {
                    if (m_durationMs > 0
                        && static_cast<uint64_t>(seekMs) > m_durationMs) {
                        // 目标超出可解内容：不新增拒绝出口，交给 EOS 汇总按
                        // m_seekBaseMs + 解出时长 与标称时长比，汇总 DataTruncated（#49）。
                        finishAtEndOfStream();
                        break;
                    }
                    setError(sdk::ErrorCode::DataCorrupted, "flac seek failed");
                    break;
                }
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
            // contract（#66）：整块视图回退口已退场——Vorbis 的 DataSource 缺席时
            // 按初始化失败汇总，不再持有整首 payload。
            setError(sdk::ErrorCode::DecodeInitFailed, "vorbis has no data source");
            return;
        }

        while (!m_stopRequest.load()) {
            int64_t seekMs = m_seekRequestMs.exchange(-1);
            if (seekMs >= 0) {
                m_state.store(StreamDecoderState::SEEKING);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                applySegmentBase(static_cast<uint64_t>(seekMs));
                m_ring->reset();
                m_processedBytes   = 0;
                m_seekDiscardBytes = 0;
                m_decodedFrames    = 0;
                m_seekBaseMs       = static_cast<uint64_t>(seekMs);
                if (!vorbis.seekToMs(static_cast<uint64_t>(seekMs))) {
                    if (m_durationMs > 0
                        && static_cast<uint64_t>(seekMs) > m_durationMs) {
                        // 目标超出可解内容：不新增拒绝出口，交给 EOS 汇总（#49）。
                        m_abortDecode.store(false);
                        finishAtEndOfStream();
                        break;
                    }
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

    if (m_codecID == AUDIO_CODEC_ID_NONE && m_extractor->packetCount() != 0) {
        // 逐包供给（#61）：wav / aiff 的负载是原始 PCM，不需要解码，包边界由提取器
        // 按 offset 现场切片。与 FLAC 分支同形——包游标在解码线程里创建、随任务退出释放
        // （规格 4.3）；「有包就逐包 pull」是供给形式的判据（#66 contract 后无回退口）。
        PacketCursor cursor(m_extractor);
        AudioBuffer::AudioBufferPtr packet;
        while (!m_stopRequest.load()) {
            const int64_t seekMs = m_seekRequestMs.exchange(-1);
            if (seekMs >= 0) {
                // P3 归零例外的实现层短路（#153 D21）——与 runFfmpegPacketLoop 的 seek
                // 分支同判：无限流（isLivePassthrough：durationMs==0 单一信号 + 直通
                // 提取器）上任何 seek 都静默忽略，不进 SEEKING、不改段基准。
                // cursor.rewind() 在 LIVE 上会走 packetAt(0) 的 av_seek_frame，必败
                // 且错误被 hls 洗成裸 -1 → 会把「拖回 0」误变 EOS。
                if (isLivePassthrough()) {
                    continue;
                }
                m_state.store(StreamDecoderState::SEEKING);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                // 位置口径与丢弃口径与通用分支一致：ring 里放的是「写进去的那一档」PCM，
                // m_seekDiscardBytes 数的是重采样之前的源格式 PCM，两者不互相换算。
                applySegmentBase(static_cast<uint64_t>(seekMs));
                m_ring->reset();
                m_processedBytes = 0;
                m_seekDiscardBytes
                    = (m_bytesPerMs == 0) ? 0 : static_cast<size_t>(seekMs) * m_bytesPerMs;
                m_decodedFrames = 0;
                m_seekBaseMs    = static_cast<uint64_t>(seekMs);
                m_abortDecode.store(false);
                // 顺序重放档（规格第 6 节）：包序列回到起点，目标之前的 PCM
                // 由 writeDecodedBytes 逐字节丢掉——不新建整首 payload 缓存。
                cursor.rewind();
                m_state.store(StreamDecoderState::DECODING);
                continue;
            }

            // 背压停在解码线程：ring 写不下就先不拉下一包（连带暂停提取器）。
            if (m_ring->availableWrite() < calculateMinWriteSpace(m_codecID)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (!cursor.nextPacket(packet)) {
                // P3 单一入口（与 runFfmpegPacketLoop 同构，#149 D2 / #151 D11 / ADR 0006）：
                // EOS 之前先查一次诊断旁路（消费即失效）。LIVE（durationMs==0 单一信号）
                // 上 EOF = 供给停滞、EIO / HTTP 错 = 数据源层失败，都进重连通道；
                // AVERROR_EXIT = 取消；AVERROR_INVALIDDATA = 内容层坏数据，仍交 EOS
                // 汇总兜底（ADR 0002 不动）。VOD（durationMs>0）与无诊断提取器落到
                // 下方既有 EOS 收场，行为与今天逐字一致（D14：都记、只由消费侧分流）。
                int readErr = 0;
                const bool hasDiag = (m_extractor != nullptr)
                    && m_extractor->consumeLastReadError(&readErr);
                if (hasDiag && isLivePassthrough()
                    && readErr != AVERROR_EXIT && readErr != AVERROR_INVALIDDATA) {
                    const ReconnectResult rc = reconnectForLiveCore();
                    if (rc == kReconnectResumed) {
                        // 重连静默成功（D6）：零回调、零状态变化。reopenConnection 已把
                        // 提取器 m_consumed 归零，rewind 即从新窗口的头重新顺序迭代。
                        cursor.rewind();
                        continue;
                    }
                    if (rc == kReconnectExhausted) {
                        // 预算耗尽是唯一终局出口（D20/D9）：沿用既有 FileOpenFailed。
                        setError(sdk::ErrorCode::FileOpenFailed,
                                 "live source reconnect budget exhausted");
                        break;
                    }
                    continue;   // Interrupted：stop / 取消 / 新 seek，回主循环头处置
                }
                if (m_stopRequest.load() || m_abortDecode.load()) {
                    break;   // P1：取消导致的读中断不是 EOS，不触发曲目结束序列
                }
                finishAtEndOfStream();
                break;
            }
            // 记账挂在 alloc / free 两个事件上（#60 的口径）：同一时刻只为一首曲目持有一包。
            onPacketAcquired(packet->size());
            onPacketPulled(cursor.packetsProduced());
            const bool written = writeDecodedBytes(packet->data(), packet->size());
            onPacketReleased(packet->size());
            packet.reset();

            if (!written) {
                if (m_stopRequest.load()) {
                    break;
                }
                if (m_seekRequestMs.load() >= 0) {
                    continue;
                }
                setError(sdk::ErrorCode::DataCorrupted, "decode failed");
                break;
            }
        }

        if (m_stopRequest.load()) {
            m_state.store(StreamDecoderState::IDLE);
        }
        return;
    }

    // ffmpeg 逐包分支（#62 mp3/aac + #63 现场重算包边界的提取器）：包边界在播放期
    // 现场重算的提取器用 kUnknownPacketCount 表示总数（mkv 走这条），mp3 / aac 的
    // 帧边界由各自的帧读取器给出。两条判据是「或」：任一成立就逐包 pull。
    // wav / aiff 与 FLAC / Vorbis 各有自己的分支，已在上面返回；11 种格式全部命中
    // 上面四个增量分支（#66 contract），生产链不再有整块视图回退口。
    if (m_extractor->packetCount() == ExtractorHelper::kUnknownPacketCount
        || ((m_codecID == AUDIO_CODEC_ID_MP3 || m_codecID == AUDIO_CODEC_ID_AAC)
            && m_extractor->packetCount() != 0)) {
        runFfmpegPacketLoop();
        if (m_stopRequest.load()) {
            m_state.store(StreamDecoderState::IDLE);
        }
        return;
    }

    // contract 阶段（#66）：11 种格式全部有包工厂，生产链不再有整块视图回退口。
    // 能走到这里说明提取器既不属于上面任何增量分支（既无包工厂也无 DataSource），
    // 按解码失败汇总，避免解码线程静默退出后状态机停在 DECODING。
    setError(sdk::ErrorCode::DataCorrupted, "decode failed");

    if (m_stopRequest.load()) {
        m_state.store(StreamDecoderState::IDLE);
    }
}

void AudioStreamDecoder::onAudioDecodeCallback(AudioDecodeSpec &out)
{
    if (m_stopRequest.load()) {
        // 停止同时置取消位：让 libFLAC / libvorbis 的解码调用尽快返回。
        m_abortDecode.store(true);
        return;
    }

    // 取消位一旦置起就丢掉这一帧（不写 ring）：上层只认「取消」这一个信号，
    // 不在回调里区分是停止还是 seek——两者的处置都是「这次解码作废」。
    if (cancelRequested()) {
        return;
    }

    // 规格 6.2 S3：采样率/声道以解码首帧实测为准，且必须**无条件**覆盖。
    // 修好前这一整块被 `m_lazyResample.isInit() && !IsValueCreated()` 挡着，
    // 而工厂只在 `m_srcSpec != m_devSpec` 时才建——声明值恰好等于设备规格时，
    // 首帧实测到的就成了头里那个错值（44100 的码流被当 48000 播，快 8.8%）。
    if (m_firstDecodeFrame && out.spec.sampleRate > 0 && out.spec.numChannel > 0
        && out.spec.bytesPerSample > 0 && out.spec.format != AudioFormatUnknown) {
        m_firstDecodeFrame = false;

        AudioSpec frameSpec;
        frameSpec.sampleRate     = out.spec.sampleRate;
        frameSpec.numChannel     = out.spec.numChannel;
        frameSpec.format         = out.spec.format;
        frameSpec.bytesPerSample = out.spec.bytesPerSample;
        frameSpec.bitsPerSample  = out.spec.bitsPerSample;

        if (!(m_srcSpec == frameSpec) || m_srcSpec.bytesPerSample != frameSpec.bytesPerSample) {
            m_srcSpec.sampleRate     = frameSpec.sampleRate;
            m_srcSpec.numChannel     = frameSpec.numChannel;
            m_srcSpec.format         = frameSpec.format;
            m_srcSpec.bytesPerSample = frameSpec.bytesPerSample;
            m_srcSpec.bitsPerSample  = frameSpec.bitsPerSample;
            m_bytesPerMs             = static_cast<uint64_t>(m_srcSpec.sampleRate)
                * static_cast<uint64_t>(m_srcSpec.numChannel)
                * static_cast<uint64_t>(m_srcSpec.bytesPerSample) / 1000;

            // 覆盖后按新规格重建：原先可能已按声明值建过，留着就是拿错参数做重采样。
            // 与设备规格相等就把工厂丢掉（不需要重采样）。
            m_lazyResample = Optional<Lazy<std::shared_ptr<AudioResample>>>();
            if (!(m_srcSpec == m_devSpec) && deviceSpecSupportsResample()) {
                m_lazyResample.emplace(
                    [this]() -> std::shared_ptr<AudioResample> { return createResampler(); });
                if (m_lazyResample->Value() == nullptr) {
                    setError(sdk::ErrorCode::DecodeInitFailed, "resample init failed");
                    return;
                }
            }
        }
    }

    // seek 发生在首帧之前：此刻字节口径已经确定，把当时寄存的段基准补上。
    if (m_pendingSeekBaseMs != 0) {
        const uint64_t bytesPerMs = positionBytesPerMs();
        if (bytesPerMs != 0) {
            m_segmentBaseBytes.store(m_pendingSeekBaseMs * bytesPerMs);
            m_pendingSeekBaseMs = 0;
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
        if (cancelRequested()) {
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

    // 观测点：start() → 首次把 PCM 交给消费端（ring 写入成功）。
    // 只记第一次；seek 之后的重新起算不属于首播延迟（规格第 5 节）。
    if (!m_firstPcmNoted && written > 0) {
        m_firstPcmNoted = true;
        const std::chrono::steady_clock::duration elapsed
            = std::chrono::steady_clock::now() - m_startClock;
        m_startToFirstPcmUs.store(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        // 并行判据的结构证据：首帧交付那一刻供给序列走到哪。整块形式下这里等于总数。
        m_packetsPulledAtFirstPcm.store(m_packetsPulled.load());
        refreshHeldPeak();
    }

    m_processedBytes += (pcmSize - skipped);
    return true;
}


std::unique_ptr<AudioDecode> AudioStreamDecoder::createFfmpegPacketDecoder()
{
    AudioCodecConfig config;
    config.sampleRate    = m_srcSpec.sampleRate;
    config.channels      = m_srcSpec.numChannel;
    config.bitsPerSample = m_srcSpec.bitsPerSample;
    config.bitRate       = m_extractor != nullptr ? m_extractor->getBitRate() : 0;
    config.blockAlign    = m_extractor != nullptr ? m_extractor->getBlockAlign() : 0;
    config.extraData     = m_extractor != nullptr ? m_extractor->getCodecExtraData() : nullptr;

    std::unique_ptr<AudioDecode> decode(new AudioDecode(m_codecID, this, config));
    if (decode->initCheck() != sdk_utils::OK) {
        LOGE("ffmpeg packet decoder init failed, codec=%#x", m_codecID);
        return std::unique_ptr<AudioDecode>();
    }
    return decode;
}

void AudioStreamDecoder::runFfmpegPacketLoop()
{
    // 编解码器跨整个播放任务存活（规格 4.3 所有权清单）；demux 面（包游标 + 扫描缓冲）
    // 同样创建在解码线程里、随本任务退出释放。
    std::unique_ptr<AudioDecode> decode = createFfmpegPacketDecoder();
    if (decode == nullptr) {
        setError(sdk::ErrorCode::DecodeInitFailed, "ffmpeg packet decoder init failed");
        return;
    }

    // 新任务从流头开始：提取器可能还停在上一任务读到的位置。
    m_extractor->seekPacketByte(0);
    std::unique_ptr<PacketCursor> cursor(new PacketCursor(m_extractor));

    for (;;) {
        const int64_t seekMs = m_seekRequestMs.exchange(-1);
        if (seekMs >= 0) {
            // P3 归零例外的实现层短路（#153 D21）：无限流（isLivePassthrough）上
            // 任何 seek 都静默忽略——不进 SEEKING、不改段基准、不回调。理由：直通路径的
            // 重放走 packetAt(0) 的 av_seek_frame，LIVE 上必然失败且错误被 hls 洗成裸 -1
            // → nullptr → 会把「拖回 0」误变曲目结束（EOS）。公开契约不动：durationMs==0
            // && pos>0 的空操作已由 MusicPlayer 层 pos > durationMs 保证（一字不动），
            // 这里补的是 pos==0 穿透后的实现层短路，不是改 §7 公开契约。
            if (isLivePassthrough()) {
                continue;
            }
            m_state.store(StreamDecoderState::SEEKING);
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            applySegmentBase(static_cast<uint64_t>(seekMs));
            m_ring->reset();
            m_processedBytes   = 0;
            m_seekDiscardBytes = (m_bytesPerMs == 0)
                ? 0
                : static_cast<size_t>(seekMs) * m_bytesPerMs;
            m_decodedFrames = 0;
            m_seekBaseMs    = static_cast<uint64_t>(seekMs);
            m_abortDecode.store(false);
            m_state.store(StreamDecoderState::DECODING);

            // 顺序重放（#49 第二档）：重建编解码器，从包序列起点重新迭代到目标，
            // 目标之前的 PCM 由 writeDecodedBytes 丢弃。峰值不随目标位置增长。
            m_extractor->seekPacketByte(0);
            cursor.reset(new PacketCursor(m_extractor));
            decode = createFfmpegPacketDecoder();
            if (decode == nullptr) {
                setError(sdk::ErrorCode::DataCorrupted, "ffmpeg packet decoder reinit failed");
                return;
            }
            continue;
        }

        if (m_stopRequest.load()) {
            break;
        }
        // 取消位已置但还没取到 seek 目标：可能是 stop（上面已处理）或新 seek 正在路上。
        if (m_abortDecode.load()) {
            if (m_seekRequestMs.load() >= 0) {
                continue;
            }
            break;
        }

        // 背压只停在解码线程：ring 写不下就暂停拉包（连带暂停 demux）。
        if (m_ring->availableWrite() < calculateMinWriteSpace(m_codecID)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        AudioBuffer::AudioBufferPtr packet;
        if (!cursor->nextPacket(packet)) {
            // P3 单一入口（#149 D2 / #151 D11 / ADR 0006）：flush 之前先查一次诊断旁路
            // （消费即失效）。LIVE（durationMs==0 单一信号，#151 D12）上 EOF = 供给停滞、
            // EIO / HTTP 错 = 数据源层失败，都进重连通道；AVERROR_EXIT = 取消（不重连）；
            // AVERROR_INVALIDDATA = 内容层坏数据，仍交播放期 EOS 汇总（ADR 0002 不动）。
            // VOD（durationMs>0）与无诊断提取器 hasDiag=false → 落到下方既有 EOS 汇总，
            // 行为与今天逐字一致（D14：都记、只由消费侧分流）。
            // 取消检查优先级高于任何分类：stop / abort 的 break 在下方既有检查点，
            // 不产生第二条取消通道。
            int readErr = 0;
            const bool hasDiag = (m_extractor != nullptr)
                && m_extractor->consumeLastReadError(&readErr);
            if (hasDiag && isLivePassthrough()
                && readErr != AVERROR_EXIT && readErr != AVERROR_INVALIDDATA) {
                const ReconnectResult rc = reconnectForLive(cursor, decode);
                if (rc == kReconnectResumed) {
                    continue;   // 重连静默成功（D6）：零回调、零状态变化，继续按已消费 PCM 推进
                }
                if (rc == kReconnectExhausted) {
                    // 预算耗尽是唯一终局出口（D20/D9）：沿用既有 FileOpenFailed →
                    // error → state，不新增错误码、不逐次上报重试失败。
                    setError(sdk::ErrorCode::FileOpenFailed,
                             "live source reconnect budget exhausted");
                    break;
                }
                // kReconnectInterrupted：stop / 取消 / 新 seek —— 静默回主循环头，
                // 由既有检查处置（stop → break；seek → 取槽处理）。
                continue;
            }

            // 序列耗尽：先清掉编解码器内部延迟，再做 EOS 汇总。
            const int flushRet = decode->flushDecode();
            if (m_stopRequest.load()) {
                break;
            }
            if (m_seekRequestMs.load() >= 0) {
                m_abortDecode.store(false);
                continue;
            }
            if (flushRet < 0) {
                setError(sdk::ErrorCode::DataCorrupted, "decode flush failed");
                break;
            }
            finishAtEndOfStream();
            break;
        }

        // 分配记账挂在 alloc/free 事件上（规格第 5 节）：同一时刻只为一首曲目持一包。
        onPacketAcquired(packet->size());
        onPacketPulled(cursor->packetsProduced());
        const int ret = decode->decodePacket(packet);
        onPacketReleased(packet->size());

        if (ret < 0) {
            if (m_stopRequest.load()) {
                break;
            }
            if (m_seekRequestMs.load() >= 0) {
                m_abortDecode.store(false);
                continue;
            }
            setError(sdk::ErrorCode::DataCorrupted, "decode failed");
            break;
        }
    }
}

AudioStreamDecoder::ReconnectResult
AudioStreamDecoder::reconnectForLiveCore()
{
    // P3 重连状态机（doc/live-source-spec.md §5.2/§5.3，#149/#150 / ADR 0007）：
    // 状态机（预算、等待、终局）归解码线程；提取器只执行无状态重开原语。
    // 本函数只做预算 + 重开 + 规格一致性准入，不含分支重建（调用方各自完成）。
    //
    // 预算（D8/D9，策略参数非 CI 门禁阈值）：有限重连 3 次、每次尝试前固定等待
    // 1 s（100 ms 分片 ×10，轮询取消，不裸 sleep）、单次窗口 ≤4 s（1 s 等待 +
    // 3 s rw_timeout 墙钟 + O(1) 一致性判定）、总预算 ≤12 s（含重开窗口）。
    // 第 3 次失败即耗尽，没有额外最后一次。
    // 停止上界（D10）：stop() 的唯一硬保证是 100 ms 取消粒度——等待期由分片轮询
    // 打断；avformat_open_input / find_stream_info 的阻塞由同一 interrupt_callback
    // 打断（rw_timeout=3s 是播放主路径唯一墙钟，#155 D15）。总预算不是 stop 上界。
    // 不托管 FFmpeg 原生 reconnect*（D4）：预算不可计量、取消不可打断、hls 不下传。
    for (int attempt = 0; attempt < 3; ++attempt) {
        // 1 s 固定间隔：100 ms 分片 ×10，取消 / 新 seek 随时可打断（不裸 sleep）。
        for (int slice = 0; slice < 10; ++slice) {
            if (m_stopRequest.load() || m_abortDecode.load()
                || m_seekRequestMs.load() >= 0) {
                return kReconnectInterrupted;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (m_stopRequest.load() || m_abortDecode.load()) {
            return kReconnectInterrupted;
        }
        // 尝试计数仪表（规格 §10.2 事件计数判据的取数点，票 #159）：计数的是
        // 「真正走到重开一步」的尝试（等待期被打断的不计）。预算破坏（改常数 /
        // 去掉计数）用它直接变红，不受 FFmpeg read_data 吞错重试 burst 干扰。
        m_reconnectAttempts.fetch_add(1, std::memory_order_release);

        // 无状态重开原语（D1）：open 失败 = 本次重连失败，计入预算，不进 EOS、
        // 不重发回调（#155：两个失败面共用同一预算）。
        if (!m_extractor->reopenConnection()) {
            LOGW("live reconnect attempt %d: reopen failed", attempt + 1);
            continue;
        }

        // 规格一致性准入（D5/§5.4）：重开后的流身份（codec + AudioSpec 身份字段 +
        // 标称时长）与首次 open 一致才成功；不一致（码率 / 编解码器切换、LIVE 转
        // VOD）判本次重连失败、计入预算，不做隐式规格切换——保住 duration()==0
        // 不变与 position 单调（D12）。samples 是长度估计不是流身份，不比对；
        // extradata 需要提取器新增访问 API（HLS AAC-in-TS 带内 ADTS 场景收益
        // 有限），登记图外（票 #158 comment）。
        const AudioSpec spec = m_extractor->getAudioSpec();
        if (m_extractor->getAudioCodecID() != m_codecID
            || spec.sampleRate != m_srcSpec.sampleRate
            || spec.format != m_srcSpec.format
            || spec.numChannel != m_srcSpec.numChannel
            || spec.bitsPerSample != m_srcSpec.bitsPerSample
            || spec.bytesPerSample != m_srcSpec.bytesPerSample
            || spec.durationMs != m_srcSpec.durationMs) {
            LOGW("live reconnect attempt %d: spec mismatch, counted as failed", attempt + 1);
            continue;
        }
        LOGI("live reconnect attempt %d: reopened and admitted", attempt + 1);
        return kReconnectResumed;
    }
    return kReconnectExhausted;
}

AudioStreamDecoder::ReconnectResult
AudioStreamDecoder::reconnectForLive(std::unique_ptr<PacketCursor> &cursor,
                                     std::unique_ptr<AudioDecode> &decode)
{
    // ffmpeg 逐包分支（mp3 / aac 等）的重连包装：core 通过后重建包游标与解码器
    // （复用 seek 分支的现成写法）。PCM 逐包分支（wav / aiff）在同文件内直接调
    // reconnectForLiveCore + cursor.rewind()（重开已把提取器 m_consumed 归零）。
    const ReconnectResult rc = reconnectForLiveCore();
    if (rc != kReconnectResumed) {
        return rc;
    }
    cursor.reset(new PacketCursor(m_extractor));
    decode = createFfmpegPacketDecoder();
    if (decode == nullptr) {
        // 重建失败（实现 / 资源层病态，非网络失败面）：网络失败在 core 里已计满
        // 预算；重建失败不重复扣预算，按耗尽终局（FileOpenFailed）。
        return kReconnectExhausted;
    }
    return kReconnectResumed;
}
