#include "MKVExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <climits>
#include <cstring>
#include <vector>
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
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
    , m_metaBuf(nullptr)
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

    std::vector<uint8_t> audioData;
    if (fileSize > 0 && fileSize < INT32_MAX) {
        audioData.reserve(static_cast<size_t>(fileSize));
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_MEMORY;
    }

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audioIndex && pkt->size > 0) {
            size_t oldSize = audioData.size();
            audioData.resize(oldSize + static_cast<size_t>(pkt->size));
            memcpy(audioData.data() + oldSize, pkt->data, pkt->size);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    avformat_close_input(&fmt);
    avio_context_free(&avioCtx);

    if (audioData.empty()) {
        LOGW("initWithFFmpegDemux no audio payload collected");
        return NO_INIT;
    }

    m_metaBuf = std::make_shared<AudioBuffer>(audioData.size());
    memcpy(m_metaBuf->data(), audioData.data(), audioData.size());
    m_validFormat = true;
    LOGI("initWithFFmpegDemux ok, codec=%#x, payload=%zu", m_audioCodecID, audioData.size());
    normalizeAudioSpec(m_audioSpec);
    return OK;
}
