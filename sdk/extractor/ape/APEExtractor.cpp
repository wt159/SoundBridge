#include "APEExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include "ByteUtils.h"
#include <climits>
#include <cstring>
#include <vector>
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
}

#define LOG_TAG "APEExtractor"

using namespace sdk_utils;

// APE version 3.98+ uses the descriptor/header layout we parse here.
static const uint16_t kApeMinVersion = 3980;
// APE descriptor is 52 bytes for v3.98+.
static const size_t kApeDescriptorBytes = 52;
// APE header is at least 24 bytes (fixed portion) and can include optional fields.
static const uint32_t kApeHeaderMinBytes = 24;
// Safety cap to avoid reading unreasonable header sizes from a corrupt file.
static const uint32_t kApeHeaderMaxBytes = 1024;

static bool parseApeHeader(DataSourceBase *source, int &sampleRate, int &channels, int &bitsPerSample)
{
    uint8_t desc[kApeDescriptorBytes] = { 0 };
    if (!source || source->readAt(0, desc, sizeof(desc)) < static_cast<ssize_t>(sizeof(desc))) {
        return false;
    }
    if (memcmp(desc, "MAC ", 4) != 0) {
        return false;
    }

    uint16_t version = U16LE_AT(desc + 4);
    if (version < kApeMinVersion) {
        return false;
    }

    uint32_t descriptorBytes = U32LE_AT(desc + 8);
    uint32_t headerBytes     = U32LE_AT(desc + 12);
    if (descriptorBytes < sizeof(desc) || headerBytes < kApeHeaderMinBytes || headerBytes > kApeHeaderMaxBytes) {
        return false;
    }

    std::vector<uint8_t> header(headerBytes);
    if (source->readAt(descriptorBytes, header.data(), headerBytes)
        < static_cast<ssize_t>(headerBytes)) {
        return false;
    }

    uint16_t bits = U16LE_AT(header.data() + 16);
    uint16_t ch   = U16LE_AT(header.data() + 18);
    uint32_t sr   = U32LE_AT(header.data() + 20);

    if (bits > 0) {
        bitsPerSample = bits;
    }
    if (ch > 0) {
        channels = ch;
    }
    if (sr > 0) {
        sampleRate = static_cast<int>(sr);
    }
    return (bitsPerSample > 0 && channels > 0 && sampleRate > 0);
}

static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool APEExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[4] = {0};
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    return memcmp(buf, "MAC ", 4) == 0;
}

APEExtractor::APEExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_metaBuf(nullptr)
    , m_codecExtraData(nullptr)
    , m_bitRate(0)
    , m_blockAlign(0)
{
    m_initCheck = initWithFFmpegDemux();
}

APEExtractor::~APEExtractor() { }

int APEExtractor::avioRead(void *opaque, uint8_t *buf, int buf_size)
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

int64_t APEExtractor::avioSeek(void *opaque, int64_t offset, int whence)
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

status_t APEExtractor::initWithFFmpegDemux()
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
    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(ioBufferSize));
    if (!ioBuffer) {
        LOGE("initWithFFmpegDemux av_malloc failed");
        return NO_MEMORY;
    }

    AVIOContext *avioCtx = avio_alloc_context(ioBuffer, ioBufferSize, 0, &ioCtxData,
                                              &APEExtractor::avioRead, nullptr,
                                              &APEExtractor::avioSeek);
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
    fmt->pb = avioCtx;
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
    m_audioCodecID = static_cast<AudioCodecID>(par->codec_id);
    m_audioSpec.sampleRate    = par->sample_rate;
    m_audioSpec.numChannel    = par->channels;
    m_audioSpec.bitsPerSample = par->bits_per_coded_sample > 0
        ? par->bits_per_coded_sample
        : par->bits_per_raw_sample;
    if (m_audioSpec.bitsPerSample == 0 || m_audioSpec.sampleRate == 0 || m_audioSpec.numChannel == 0) {
        int parsedSampleRate  = m_audioSpec.sampleRate;
        int parsedChannels    = m_audioSpec.numChannel;
        int parsedBitsPerSample = m_audioSpec.bitsPerSample;
        if (parseApeHeader(m_dataSource, parsedSampleRate, parsedChannels, parsedBitsPerSample)) {
            m_audioSpec.sampleRate   = parsedSampleRate;
            m_audioSpec.numChannel   = parsedChannels;
            m_audioSpec.bitsPerSample = parsedBitsPerSample;
            LOGI("parseApeHeader ok: sr=%d ch=%d bps=%d",
                 m_audioSpec.sampleRate, m_audioSpec.numChannel, m_audioSpec.bitsPerSample);
        } else {
            LOGW("parseApeHeader failed or incomplete, sr=%d ch=%d bps=%d",
                 m_audioSpec.sampleRate, m_audioSpec.numChannel, m_audioSpec.bitsPerSample);
        }
    }
    m_audioSpec.bytesPerSample = m_audioSpec.bitsPerSample > 0
        ? (m_audioSpec.bitsPerSample + 7) / 8
        : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
    m_bitRate    = static_cast<int>(par->bit_rate);
    m_blockAlign = static_cast<int>(par->block_align);
    if (m_blockAlign == 0 && m_audioSpec.numChannel > 0 && m_audioSpec.bitsPerSample > 0) {
        m_blockAlign = m_audioSpec.numChannel * (m_audioSpec.bitsPerSample / 8);
    }
    if (m_bitRate == 0 && fmt->duration > 0 && fileSize > 0) {
        double seconds = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        if (seconds > 0.0) {
            m_bitRate = static_cast<int>((fileSize * 8.0) / seconds);
        }
    }
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
    return OK;
}
