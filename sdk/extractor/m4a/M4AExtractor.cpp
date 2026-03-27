#include "M4AExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <climits>
#include <cstring>
#include <vector>
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
}

#define LOG_TAG "M4AExtractor"

using namespace sdk_utils;

static int sampleRateToIndex(int sampleRate)
{
    static const int kSampleRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000,  7350 };
    for (int i = 0; i < (int)(sizeof(kSampleRates) / sizeof(kSampleRates[0])); ++i) {
        if (kSampleRates[i] == sampleRate) {
            return i;
        }
    }
    return -1;
}

static bool parseAudioSpecificConfig(const uint8_t *data, int size, int &audioObjectType,
                                     int &sampleRateIndex, int &channelConfig)
{
    if (!data || size < 2) {
        return false;
    }
    int bitpos   = 0;
    auto getBits = [&](int n) -> int {
        int out = 0;
        for (int i = 0; i < n; ++i) {
            int byte = (bitpos >> 3);
            int bit  = 7 - (bitpos & 7);
            if (byte >= size) {
                return -1;
            }
            out = (out << 1) | ((data[byte] >> bit) & 1);
            bitpos++;
        }
        return out;
    };

    int aot = getBits(5);
    if (aot < 0) {
        return false;
    }
    if (aot == 31) {
        int ext = getBits(6);
        if (ext < 0) {
            return false;
        }
        aot = 32 + ext;
    }
    int srIndex = getBits(4);
    if (srIndex < 0) {
        return false;
    }
    if (srIndex == 0x0F) {
        int sr = getBits(24);
        if (sr < 0) {
            return false;
        }
        srIndex = sampleRateToIndex(sr);
    }
    int ch = getBits(4);
    if (ch < 0) {
        return false;
    }

    audioObjectType = aot;
    sampleRateIndex = srIndex;
    channelConfig   = ch;
    return true;
}

static void buildAdtsHeader(uint8_t *out, int profile, int sampleRateIndex, int channelConfig,
                            int frameLen)
{
    // profile: 0 = Main, 1 = LC, 2 = SSR, 3 = LTP
    if (profile < 0) {
        profile = 1;
    }
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (uint8_t)(((profile & 0x03) << 6) | ((sampleRateIndex & 0x0F) << 2)
                       | ((channelConfig >> 2) & 0x01));
    out[3] = (uint8_t)(((channelConfig & 0x03) << 6) | ((frameLen >> 11) & 0x03));
    out[4] = (uint8_t)((frameLen >> 3) & 0xFF);
    out[5] = (uint8_t)(((frameLen & 0x07) << 5) | 0x1F);
    out[6] = 0xFC;
}

static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool M4AExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[12] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    return memcmp(buf + 4, "ftyp", 4) == 0;
}

M4AExtractor::M4AExtractor(DataSourceBase *source)
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

M4AExtractor::~M4AExtractor() { }

int M4AExtractor::avioRead(void *opaque, uint8_t *buf, int buf_size)
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

int64_t M4AExtractor::avioSeek(void *opaque, int64_t offset, int whence)
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

status_t M4AExtractor::initWithFFmpegDemux()
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
        = avio_alloc_context(ioBuffer, ioBufferSize, 0, &ioCtxData, &M4AExtractor::avioRead,
                             nullptr, &M4AExtractor::avioSeek);
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

    bool useAdts = (par->codec_id == AV_CODEC_ID_AAC);
    int aot      = 2;
    int srIndex  = sampleRateToIndex(par->sample_rate);
    int chCfg    = par->channels;
    if (useAdts && par->extradata && par->extradata_size > 0) {
        int paot = 0, psr = -1, pch = 0;
        if (parseAudioSpecificConfig(par->extradata, par->extradata_size, paot, psr, pch)) {
            aot = paot;
            if (psr >= 0) {
                srIndex = psr;
            }
            if (pch > 0) {
                chCfg = pch;
            }
        }
    }
    if (useAdts && srIndex < 0) {
        useAdts = false;
    }

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audioIndex && pkt->size > 0) {
            if (useAdts) {
                int profile = aot - 1;
                if (aot == 5 || aot == 29) {
                    profile = 1; // use AAC LC in ADTS
                }
                uint8_t adts[7];
                int frameLen = pkt->size + 7;
                buildAdtsHeader(adts, profile, srIndex, chCfg, frameLen);
                size_t oldSize = audioData.size();
                audioData.resize(oldSize + 7 + static_cast<size_t>(pkt->size));
                memcpy(audioData.data() + oldSize, adts, 7);
                memcpy(audioData.data() + oldSize + 7, pkt->data, pkt->size);
            } else {
                size_t oldSize = audioData.size();
                audioData.resize(oldSize + static_cast<size_t>(pkt->size));
                memcpy(audioData.data() + oldSize, pkt->data, pkt->size);
            }
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
