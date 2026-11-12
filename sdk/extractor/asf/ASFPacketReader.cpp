#include "ASFPacketReader.h"
#include "LogWrapper.h"
#include "AbortInterrupt.hpp"
#include <chrono>
#include <climits>
#include <cstring>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/rational.h"
}

#define LOG_TAG "ASFPacketReader"

namespace {
// AVIO 缓冲：判据公式里的固定项（64 KB），与素材时长无关。
const int kIoBufferSize = 64 * 1024;
} // namespace

ASFStreamInfo::ASFStreamInfo()
    : codecId(0)
    , sampleRate(0)
    , channels(0)
    , bitsPerCodedSample(0)
    , bitsPerRawSample(0)
    , bitRate(0)
    , blockAlign(0)
    , timeBaseMs(0.0)
    , extradata()
{
}

ASFPacketReader::ASFPacketReader(DataSourceBase *source, int64_t fileSize)
    : m_ioData()
    , m_fmt(nullptr)
    , m_avio(nullptr)
    , m_pkt(nullptr)
    , m_audioIndex(-1)
    , m_lastPacketDuration(0)
    , m_boundaryMicros(0)
{
    m_ioData.source = source;
    m_ioData.pos    = 0;
    m_ioData.size   = static_cast<int64_t>(fileSize);
}

ASFPacketReader::~ASFPacketReader()
{
    close();
}

int ASFPacketReader::avioRead(void *opaque, uint8_t *buf, int buf_size)
{
    if (!opaque || !buf || buf_size <= 0) {
        return AVERROR_EOF;
    }
    AvioData *ctx = reinterpret_cast<AvioData *>(opaque);
    if (!ctx->source) {
        return AVERROR_EOF;
    }
    // P1：自定义 AVIO 的 read_packet 阻塞点。只认 m_abortDecode，
    // 绝不复用 cancelRequested()（含 seek 槽会误中断合法 seek）。
    if (sbAbortRequested(ctx->source)) {
        return AVERROR_EXIT;
    }
    ssize_t n = ctx->source->readAt(ctx->pos, buf, static_cast<size_t>(buf_size));
    if (n <= 0) {
        return AVERROR_EOF;
    }
    ctx->pos += n;
    return static_cast<int>(n);
}

int64_t ASFPacketReader::avioSeek(void *opaque, int64_t offset, int whence)
{
    if (!opaque) {
        return -1;
    }
    AvioData *ctx = reinterpret_cast<AvioData *>(opaque);
    if (!ctx->source) {
        return -1;
    }
    if (whence == AVSEEK_SIZE) {
        return ctx->size;
    }

    int64_t newPos = ctx->pos;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        newPos = offset;
        break;
    case SEEK_CUR:
        newPos = ctx->pos + offset;
        break;
    case SEEK_END:
        newPos = ctx->size + offset;
        break;
    default:
        return -1;
    }

    if (newPos < 0) {
        newPos = 0;
    }
    if (ctx->size >= 0 && newPos > ctx->size) {
        newPos = ctx->size;
    }
    ctx->pos = newPos;
    return ctx->pos;
}

bool ASFPacketReader::open()
{
    if (m_fmt != nullptr) {
        return true;
    }
    if (m_ioData.source == nullptr || m_ioData.size <= 0) {
        return false;
    }
    m_ioData.pos         = 0;
    m_lastPacketDuration = 0;

    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(kIoBufferSize));
    if (!ioBuffer) {
        LOGE("av_malloc failed");
        return false;
    }

    m_avio = avio_alloc_context(ioBuffer, kIoBufferSize, 0, &m_ioData,
                                &ASFPacketReader::avioRead, nullptr,
                                &ASFPacketReader::avioSeek);
    if (!m_avio) {
        av_free(ioBuffer);
        LOGE("avio_alloc_context failed");
        return false;
    }

    m_fmt = avformat_alloc_context();
    if (!m_fmt) {
        avio_context_free(&m_avio);
        LOGE("avformat_alloc_context failed");
        return false;
    }
    m_fmt->pb     = m_avio;
    m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // P1：让 FFmpeg 在 avformat_open_input / av_read_frame 的阻塞点也能看到同一取消信号。
    // opaque 直接挂 DataSourceBase*，不改 AvioData、不新增状态。
    m_fmt->interrupt_callback.callback = &sbAbortInterruptCallback;
    m_fmt->interrupt_callback.opaque   = m_ioData.source;

    int ret = avformat_open_input(&m_fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        LOGW("avformat_open_input failed: %d", ret);
        // open_input 失败时自己释放了 m_fmt，把口径归一成 close() 能处理的状态。
        m_fmt = nullptr;
        avio_context_free(&m_avio);
        return false;
    }

    ret = avformat_find_stream_info(m_fmt, nullptr);
    if (ret < 0) {
        LOGW("avformat_find_stream_info failed: %d", ret);
        close();
        return false;
    }

    m_audioIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audioIndex < 0) {
        LOGW("no audio stream");
        close();
        return false;
    }

    m_pkt = av_packet_alloc();
    if (!m_pkt) {
        LOGE("av_packet_alloc failed");
        close();
        return false;
    }
    return true;
}

void ASFPacketReader::close()
{
    if (m_pkt != nullptr) {
        av_packet_free(&m_pkt);
    }
    if (m_fmt != nullptr) {
        avformat_close_input(&m_fmt);
    }
    if (m_avio != nullptr) {
        avio_context_free(&m_avio);
    }
    m_audioIndex = -1;
}

bool ASFPacketReader::streamInfo(ASFStreamInfo &info) const
{
    if (m_fmt == nullptr || m_audioIndex < 0 || m_audioIndex >= static_cast<int>(m_fmt->nb_streams)) {
        return false;
    }
    AVStream *audioStream = m_fmt->streams[m_audioIndex];
    if (audioStream == nullptr || audioStream->codecpar == nullptr) {
        return false;
    }
    AVCodecParameters *par = audioStream->codecpar;
    info.codecId           = static_cast<int>(par->codec_id);
    info.sampleRate        = par->sample_rate;
    info.channels          = par->channels;
    info.bitsPerCodedSample = par->bits_per_coded_sample;
    info.bitsPerRawSample   = par->bits_per_raw_sample;
    info.bitRate            = static_cast<int>(par->bit_rate);
    info.blockAlign         = static_cast<int>(par->block_align);
    info.timeBaseMs         = av_q2d(audioStream->time_base) * 1000.0;
    info.extradata.clear();
    if (par->extradata != nullptr && par->extradata_size > 0) {
        info.extradata.assign(par->extradata, par->extradata + par->extradata_size);
    }
    return true;
}

bool ASFPacketReader::nextPacket(AudioBuffer::AudioBufferPtr &packet)
{
    packet.reset();
    if (!isOpen() && !open()) {
        return false;
    }

    // 现场重算的 CPU 成本只记独立指标（规格第 5 节）：量的是 demux 把帧 / 块头
    // 解析成边界的那一段，不含下面的负载拷贝（拷贝由解码侧的 alloc/free 记账）。
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    int ret = 0;
    for (;;) {
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            break;
        }
        if (m_pkt->stream_index == m_audioIndex && m_pkt->size > 0) {
            break;
        }
        av_packet_unref(m_pkt);
    }
    const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - begin;
    m_boundaryMicros.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));

    if (ret < 0) {
        // 包耗尽与坏数据都走到这里：可解性不在提取器判定（ADR 0002），
        // 交给播放期的「包耗尽 + EOS 汇总」兜底，能解多少播多少。
        return false;
    }

    m_lastPacketDuration = m_pkt->duration;
    AudioBuffer::AudioBufferPtr out
        = std::make_shared<AudioBuffer>(static_cast<size_t>(m_pkt->size));
    std::memcpy(out->data(), m_pkt->data, static_cast<size_t>(m_pkt->size));
    av_packet_unref(m_pkt);
    packet = out;
    return true;
}

bool ASFPacketReader::seekTo(int64_t offset)
{
    // 顺序重放档（#49 第二档）只用到 0：整个 demux 关掉重开，等价于「从流头重新迭代」。
    // 任意偏移走 demux 的字节 seek，可用时不必自己维护一张 seek 表。
    close();
    if (!open()) {
        return false;
    }
    if (offset <= 0) {
        return true;
    }
    if (avformat_seek_file(m_fmt, -1, INT64_MIN, offset, INT64_MAX, AVSEEK_FLAG_BYTE) < 0) {
        LOGW("byte seek to %lld failed", static_cast<long long>(offset));
        return false;
    }
    avformat_flush(m_fmt);
    return true;
}

int64_t ASFPacketReader::position() const
{
    if (m_fmt == nullptr || m_fmt->pb == nullptr) {
        return 0;
    }
    return static_cast<off64_t>(avio_tell(m_fmt->pb));
}

int64_t ASFPacketReader::declaredDurationUs() const
{
    return m_fmt != nullptr ? m_fmt->duration : 0;
}
