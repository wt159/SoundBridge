#include "MKVExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <climits>
#include <cstring>
#include <vector>
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/rational.h"
}

#define LOG_TAG "MKVExtractor"

using namespace sdk_utils;

static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool MKVExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[4] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    // EBML header: 1A 45 DF A3
    return buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3;
}

MKVExtractor::MKVExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_codecExtraData(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_audioSpec()
    , m_bitRate(0)
    , m_blockAlign(0)
{
    m_initCheck = initWithFFmpegDemux();
}

MKVExtractor::~MKVExtractor() { }

int MKVExtractor::avioRead(void *opaque, uint8_t *buf, int buf_size)
{
    if (!opaque || !buf || buf_size <= 0) {
        return AVERROR_EOF;
    }
    AvioData *ctx = reinterpret_cast<AvioData *>(opaque);
    if (!ctx->source) {
        return AVERROR_EOF;
    }
    ssize_t n = ctx->source->readAt(ctx->pos, buf, static_cast<size_t>(buf_size));
    if (n <= 0) {
        return AVERROR_EOF;
    }
    ctx->pos += n;
    return static_cast<int>(n);
}

int64_t MKVExtractor::avioSeek(void *opaque, int64_t offset, int whence)
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

status_t MKVExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }

    AvioData ioCtxData;
    ioCtxData.source = m_dataSource;
    ioCtxData.pos    = 0;
    ioCtxData.size   = static_cast<int64_t>(fileSize);

    const int ioBufferSize = 64 * 1024;
    uint8_t *ioBuffer      = static_cast<uint8_t *>(av_malloc(ioBufferSize));
    if (!ioBuffer) {
        LOGE("initWithFFmpegDemux av_malloc failed");
        return NO_MEMORY;
    }

    AVIOContext *avioCtx
        = avio_alloc_context(ioBuffer, ioBufferSize, 0, &ioCtxData, &MKVExtractor::avioRead,
                             nullptr, &MKVExtractor::avioSeek);
    if (!avioCtx) {
        av_free(ioBuffer);
        LOGE("initWithFFmpegDemux avio_alloc_context failed");
        return NO_MEMORY;
    }

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        avio_context_free(&avioCtx);
        LOGE("initWithFFmpegDemux avformat_alloc_context failed");
        return NO_MEMORY;
    }
    fmt->pb     = avioCtx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        LOGW("initWithFFmpegDemux avformat_open_input failed: %d", ret);
        avformat_free_context(fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        LOGW("initWithFFmpegDemux avformat_find_stream_info failed: %d", ret);
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    int audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex < 0) {
        LOGW("initWithFFmpegDemux no audio stream");
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    AVStream *audioStream = fmt->streams[audioIndex];
    if (!audioStream || !audioStream->codecpar) {
        LOGW("initWithFFmpegDemux invalid audio stream");
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    AVCodecParameters *par = audioStream->codecpar;
    m_audioCodecID         = static_cast<AudioCodecID>(par->codec_id);
    m_audioSpec.sampleRate = par->sample_rate;
    m_audioSpec.numChannel = par->channels;
    m_audioSpec.bitsPerSample
        = par->bits_per_coded_sample > 0 ? par->bits_per_coded_sample : par->bits_per_raw_sample;
    m_audioSpec.bytesPerSample
        = m_audioSpec.bitsPerSample > 0 ? (m_audioSpec.bitsPerSample + 7) / 8 : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
    m_bitRate          = static_cast<int>(par->bit_rate);
    m_blockAlign       = static_cast<int>(par->block_align);
    if (par->extradata && par->extradata_size > 0) {
        m_codecExtraData = std::make_shared<AudioBuffer>(par->extradata_size);
        memcpy(m_codecExtraData->data(), par->extradata, par->extradata_size);
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_MEMORY;
    }

    // 逐包累加时长：直接用包时间轴算真实内容长度，不读容器声明值（见下方注释）。
    int64_t totalPktDuration = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audioIndex && pkt->size > 0) {
            // 只保留解复用边界，不另拼整块（#17）：MKV 里可能是无 parser 的 codec
            //（ape/alac/pcm），整块送进去会被当成一个 packet，只能解出开头。
            AudioBuffer::AudioBufferPtr packet
                = std::make_shared<AudioBuffer>(static_cast<size_t>(pkt->size));
            memcpy(packet->data(), pkt->data, static_cast<size_t>(pkt->size));
            m_packetizedMetaBuf.push_back(packet);

            if (pkt->duration > 0) {
                totalPktDuration += pkt->duration;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // 时长归属（#15）：以「逐包时长累加」为准，容器声明值只作兜底。
    // Matroska 的 Segment/Duration 是封装器写进去的，写法出错的工具真实存在——
    // music-mkv.mkv 就把 393216 个采样当毫秒写成 393.216s，实际只有 24.576s。
    // 包时长来自解复用出的时间轴，不受该字段污染，且解析成本已经付过（本就在读包）。
    uint64_t measuredMs = 0;
    if (totalPktDuration > 0) {
        const double scale = av_q2d(audioStream->time_base) * 1000.0;
        if (scale > 0) {
            measuredMs = static_cast<uint64_t>(totalPktDuration * scale + 0.5);
        }
    }

    if (measuredMs > 0) {
        m_audioSpec.durationMs = measuredMs;
    } else if (fmt->duration > 0) {
        m_audioSpec.durationMs = static_cast<uint64_t>(fmt->duration / 1000);
    }

    avformat_close_input(&fmt);
    avio_context_free(&avioCtx);

    if (m_packetizedMetaBuf.empty()) {
        LOGW("initWithFFmpegDemux no audio payload collected");
        return NO_INIT;
    }

    m_validFormat = true;
    LOGI("initWithFFmpegDemux ok, codec=%#x, packets=%zu", m_audioCodecID,
         m_packetizedMetaBuf.size());
    normalizeAudioSpec(m_audioSpec);
    return OK;
}
