#include "AudioDecode.h"
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
}
#include "LogWrapper.h"
#include <string>

#define LOG_TAG "AudioDecode"

class AudioDecode::Impl {
private:
    AudioCodecID m_codecID;
    AVCodecID m_avCodecID;
    AVCodec *m_codec;
    AVCodecContext *m_ctx;
    AVCodecParserContext *m_parserCtx;
    AVFrame *m_frame;
    AVPacket *m_pkt;
    char m_error[AV_ERROR_MAX_STRING_SIZE];
    AudioDecodeCallback *m_callback;
    std::string m_versionInfo;
    int m_deviceVersion;
    std::string m_configuration;
    std::string m_licenseInfo;
    status_t m_initCheck;

public:
    Impl(AudioCodecID codec, AudioDecodeCallback *callback);
    ~Impl();
    int decode(const char *data, ssize_t size);
    status_t initCheck() { return m_initCheck; }

private:
    char *getAVErrorString(int errnum);
    int decode(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame);
};

AudioDecode::Impl::Impl(AudioCodecID codec, AudioDecodeCallback *callback)
    : m_codecID(codec)
    , m_avCodecID((AVCodecID)codec)
    , m_codec(nullptr)
    , m_ctx(nullptr)
    , m_parserCtx(nullptr)
    , m_frame(nullptr)
    , m_pkt(nullptr)
    , m_callback(callback)
    , m_versionInfo(av_version_info())
    , m_deviceVersion(avcodec_version())
    , m_configuration(avcodec_configuration())
    , m_licenseInfo(avcodec_license())
    , m_initCheck(NO_INIT)
{

    LOG_INFO(LOG_TAG, "av log level: %d", av_log_get_level());
    av_log_set_level(AV_LOG_TRACE);
    LOG_INFO(LOG_TAG, "av log level: %d", av_log_get_level());
    av_log_set_callback([](void *avcl, int level, const char *fmt, va_list vl) {
        char line[1024];
        vsnprintf(line, sizeof(line), fmt, vl);
        switch (level) {
        case AV_LOG_INFO:
            LOG_INFO(LOG_TAG, "%s", line);
            break;
        case AV_LOG_WARNING:
            LOG_WARNING(LOG_TAG, "%s", line);
            break;
        case AV_LOG_ERROR:
            LOG_ERROR(LOG_TAG, "%s", line);
            break;
        case AV_LOG_FATAL:
            LOG_FATAL(LOG_TAG, "%s", line);
            break;
        default:
            break;
        }
    });
    LOG_INFO(LOG_TAG, "ffmpeg version info: %s", m_versionInfo.c_str());
    LOG_INFO(LOG_TAG, "ffmpeg device version: %d", m_deviceVersion);
    LOG_INFO(LOG_TAG, "ffmpeg configuration: %s", m_configuration.c_str());
    LOG_INFO(LOG_TAG, "ffmpeg license info: %s", m_licenseInfo.c_str());

#if 0
    for (uint32_t n = AV_CODEC_ID_MP2; n <= AV_CODEC_ID_CODEC2; n++) {
        m_codec = avcodec_find_decoder((enum AVCodecID)n);
        if (m_codec) {
            LOG_INFO(LOG_TAG, "decoder found %s", avcodec_get_name((enum AVCodecID)n));
        } else {
            LOG_INFO(LOG_TAG, "decoder not found %s", avcodec_get_name((enum AVCodecID)n));
        }
    }
#endif
    m_codec = avcodec_find_decoder(m_avCodecID);
    if (!m_codec) {
        LOG_ERROR(LOG_TAG, "avcodec_find_decoder failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    m_parserCtx = av_parser_init(m_codec->id);
    if (!m_parserCtx) {
        LOG_ERROR(LOG_TAG, "av_parser_init failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    m_ctx = avcodec_alloc_context3(m_codec);
    if (!m_ctx) {
        LOG_ERROR(LOG_TAG, "avcodec_alloc_context3 failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    m_pkt = av_packet_alloc();
    if (!m_pkt) {
        LOG_ERROR(LOG_TAG, "av_packet_alloc failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    m_frame = av_frame_alloc();
    if (!m_frame) {
        LOG_ERROR(LOG_TAG, "av_frame_alloc failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    int ret = avcodec_open2(m_ctx, m_codec, nullptr);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "avcodec_open2 failed rc:%d, %s", ret, getAVErrorString(ret));
        return;
    }
    m_initCheck = OK;
    LOG_INFO(LOG_TAG, "exit");
}

AudioDecode::Impl::~Impl()
{
    if (m_pkt) {
        av_packet_free(&m_pkt);
        m_pkt = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_parserCtx) {
        av_parser_close(m_parserCtx);
        m_parserCtx = nullptr;
    }
    if (m_ctx) {
        avcodec_free_context(&m_ctx);
        m_ctx = nullptr;
    }
}

AudioFormat getAudioFormat(AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        return AudioFormatU8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        return AudioFormatS16;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        return AudioFormatS32;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        return AudioFormatFLT32;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
        return AudioFormatDBL64;
    default:
        return AudioFormatUnknown;
    }
    return AudioFormatUnknown;
}

int AudioDecode::Impl::decode(const char *srcData, ssize_t srcSize)
{
#define IN_DATA_SIZE        20480
#define AUDIO_REFILL_THRESH 4096
    int ret = 0, len = 0;
    int inSize = IN_DATA_SIZE > srcSize ? srcSize : IN_DATA_SIZE;
    char inBuf[IN_DATA_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    uint8_t *inPtr                                          = (uint8_t *)inBuf;
    char *srcPtr                                            = (char *)srcData;
    memcpy(inBuf, srcPtr, inSize);
    srcPtr  += inSize;
    srcSize -= inSize;

    while (inSize > 0) {
        ret = av_parser_parse2(m_parserCtx, m_ctx, &m_pkt->data, &m_pkt->size, inPtr, inSize,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret <= 0) {
            LOG_ERROR(LOG_TAG, "av_parser_parse2 failed: %s", getAVErrorString(ret));
            ret = -1;
            goto err;
        }
        inPtr  += ret;
        inSize -= ret;

        if (m_pkt->size > 0) {
            ret = decode(m_ctx, m_pkt, m_frame);
            if (ret < 0) {
                LOG_ERROR(LOG_TAG, "decode failed: %d", ret);
                goto err;
            }
        }

        if (inSize < AUDIO_REFILL_THRESH && srcSize > 0) {
            memmove(inBuf, inPtr, inSize);
            inPtr = (uint8_t *)inBuf;
            len   = (IN_DATA_SIZE - inSize) > srcSize ? srcSize : (IN_DATA_SIZE - inSize);
            memcpy(inPtr + inSize, srcPtr, len);
            inSize  += len;
            srcPtr  += len;
            srcSize -= len;
        }
    }

    m_pkt->data = nullptr;
    m_pkt->size = 0;
    ret         = decode(m_ctx, m_pkt, m_frame);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "decode failed: %s", getAVErrorString(ret));
    }
    return 0;
err:
    return ret;
}

char *AudioDecode::Impl::getAVErrorString(int errnum)
{
    av_strerror(errnum, m_error, sizeof(m_error));
    return m_error;
}

int AudioDecode::Impl::decode(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
{
    AudioDecodeSpec out;
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "avcodec_send_packet failed: %d:%s", ret, getAVErrorString(ret));
        goto end;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            goto end;
        } else if (ret < 0) {
            LOG_ERROR(LOG_TAG, "avcodec_receive_frame failed %d:%s", ret, getAVErrorString(ret));
            goto end;
        }
        out.lineData            = frame->data;
        out.lineSize            = frame->linesize;
        out.spec.numChannel     = frame->channels;
        out.spec.sampleRate     = frame->sample_rate;
        out.spec.samples        = frame->nb_samples;
        out.spec.format         = getAudioFormat((AVSampleFormat)m_ctx->sample_fmt);
        out.spec.bytesPerSample = getBytePreSampleByAudioFormat(out.spec.format);
        out.spec.bitsPerSample  = out.spec.bytesPerSample * 8;
        m_callback->onAudioDecodeCallback(out);
    }
end:
    return ret;
}

AudioDecode::AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback)
    : m_impl(new Impl(codec, callback))
{
}

AudioDecode::~AudioDecode() { }

status_t AudioDecode::initCheck()
{
    return m_impl->initCheck();
}

int AudioDecode::decode(const char *data, ssize_t size)
{
    return m_impl->decode(data, size);
}
