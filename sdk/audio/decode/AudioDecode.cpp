#include "AudioDecode.h"
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
}
#include "LogWrapper.h"
#include <cstring>
#include <string>
#include <vector>
#define LOG_TAG "AudioDecode"

using namespace sdk_utils;

namespace {
// 规格 4.3 的护栏：连续失败到这个数就放弃。没有它，一份纯乱码的 mp3 会让解码器
// 把整个文件扫一遍找同步点。取「帧数」写法（比 500ms 好实现，且与码率无关）。
constexpr int kMaxConsecutiveFailedPackets = 32;

// 只跳「单包解不出」：码流里有一段坏数据。上下文级失败（ENOMEM、格式不支持、
// 句柄坏掉）必须中止——它们不会因为换一个包就好了。
// AVERROR(EPERM)=-1 是 libavcodec 对「这个包解不出来」的老返回值（wma/asf 实测），
// 与 INVALIDDATA/EINVAL 同属数据层面的单包错误。
bool isSkippablePacketError(int err)
{
    return err == AVERROR_INVALIDDATA || err == AVERROR(EINVAL) || err == AVERROR(EPERM);
}
} // namespace

class AudioDecode::Impl {
private:
    AVCodecID m_avCodecID;
    AVCodec *m_codec;
    AVCodecContext *m_ctx;
    AVCodecParserContext *m_parserCtx;
    AVFrame *m_frame;
    AVPacket *m_pkt;
    char m_error[AV_ERROR_MAX_STRING_SIZE];
    AudioDecodeCallback *m_callback;
    AudioCodecConfig m_config;
    std::string m_versionInfo;
    int m_deviceVersion;
    std::string m_configuration;
    std::string m_licenseInfo;
    status_t m_initCheck;
    int m_skippedPackets;
    int m_consecutiveFailures;
    // 增量面用的输入暂存：ffmpeg 要求包尾有 AV_INPUT_BUFFER_PADDING_SIZE 的零填充，
    // 包工厂交出的包不带。复用同一块缓冲，容量随最大包 O(1)，不按包数增长。
    std::vector<uint8_t> m_scratch;

public:
    Impl(AudioCodecID codec, AudioDecodeCallback *callback, const AudioCodecConfig &config);
    ~Impl();
    int decode(const char *data, ssize_t size);
    int decodePackets(const std::vector<AudioBuffer::AudioBufferPtr> &packets);
    int decodePacket(const AudioBuffer::AudioBufferPtr &packet);
    int flushDecode();
    status_t initCheck() { return m_initCheck; }
    int skippedPacketCount() const { return m_skippedPackets; }

private:
    char *getAVErrorString(int errnum);
    int decode(AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame);
    // 返回 true 表示「这个包只是自己解不出，可以跳过继续」；false 表示应当中止。
    bool skipFailedPacket(int err);
};

AudioDecode::Impl::Impl(AudioCodecID codec, AudioDecodeCallback *callback,
                        const AudioCodecConfig &config)
    : m_avCodecID((AVCodecID)codec)
    , m_codec(nullptr)
    , m_ctx(nullptr)
    , m_parserCtx(nullptr)
    , m_frame(nullptr)
    , m_pkt(nullptr)
    , m_callback(callback)
    , m_config(config)
    , m_versionInfo(av_version_info())
    , m_deviceVersion(avcodec_version())
    , m_configuration(avcodec_configuration())
    , m_licenseInfo(avcodec_license())
    , m_initCheck(NO_INIT)
    , m_skippedPackets(0)
    , m_consecutiveFailures(0)
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
        std::fprintf(stderr, "AudioDecode: avcodec_find_decoder failed: %s\n",
                     avcodec_get_name(m_avCodecID));
        return;
    }
    LOG_INFO(LOG_TAG, "avcodec_find_decoder success: %s", avcodec_get_name(m_avCodecID));
    m_parserCtx = av_parser_init(m_codec->id);
    if (!m_parserCtx) {
        LOG_WARNING(LOG_TAG, "av_parser_init failed: %s, fallback to packet decode",
                    avcodec_get_name(m_avCodecID));
    }
    m_ctx = avcodec_alloc_context3(m_codec);
    if (!m_ctx) {
        LOG_ERROR(LOG_TAG, "avcodec_alloc_context3 failed: %s", avcodec_get_name(m_avCodecID));
        return;
    }
    if (m_config.channels > 0) {
        m_ctx->channels       = m_config.channels;
        m_ctx->channel_layout = av_get_default_channel_layout(m_config.channels);
    }
    if (m_config.sampleRate > 0) {
        m_ctx->sample_rate = m_config.sampleRate;
    }
    if (m_config.bitRate > 0) {
        m_ctx->bit_rate = m_config.bitRate;
    }
    if (m_config.blockAlign > 0) {
        m_ctx->block_align = m_config.blockAlign;
    }
    if (m_config.bitsPerSample > 0) {
        m_ctx->bits_per_coded_sample = m_config.bitsPerSample;
        m_ctx->bits_per_raw_sample   = m_config.bitsPerSample;
    }
    if (m_config.extraData && m_config.extraData->size() > 0) {
        m_ctx->extradata_size = (int)m_config.extraData->size();
        m_ctx->extradata
            = (uint8_t *)av_mallocz(m_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (m_ctx->extradata) {
            memcpy(m_ctx->extradata, m_config.extraData->data(), m_ctx->extradata_size);
        }
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
        std::fprintf(stderr,
                     "AudioDecode: avcodec_open2 failed rc:%d (%s) codec=%s sr=%d ch=%d blka=%d "
                     "br=%lld extra=%d\n",
                     ret, getAVErrorString(ret), avcodec_get_name(m_avCodecID), m_ctx->sample_rate,
                     m_ctx->channels, m_ctx->block_align, static_cast<long long>(m_ctx->bit_rate),
                     m_ctx->extradata_size);
        return;
    }
    m_initCheck = OK;
    LOG_INFO(LOG_TAG, "exit");
}

AudioDecode::Impl::~Impl()
{
    if (m_ctx && m_ctx->extradata) {
        av_free(m_ctx->extradata);
        m_ctx->extradata      = nullptr;
        m_ctx->extradata_size = 0;
    }
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
    if (!m_parserCtx) {
        if (!srcData || srcSize <= 0) {
            return 0;
        }
        m_pkt->data = (uint8_t *)srcData;
        m_pkt->size = (int)srcSize;
        ret         = decode(m_ctx, m_pkt, m_frame);
        if (ret < 0) {
            LOG_ERROR(LOG_TAG, "decode failed: %d", ret);
            return ret;
        }
        m_pkt->data = nullptr;
        m_pkt->size = 0;
        ret         = decode(m_ctx, m_pkt, m_frame);
        if (ret < 0) {
            LOG_ERROR(LOG_TAG, "decode flush failed: %s", getAVErrorString(ret));
            return ret;
        }
        return 0;
    }
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
            LOG_ERROR(LOG_TAG, "av_parser_parse2 failed: %s, rc:%d, srcSize:%ld",
                      getAVErrorString(ret), ret, srcSize);
            ret = -1;
            goto err;
        }
        inPtr  += ret;
        inSize -= ret;

        if (m_pkt->size > 0) {
            ret = decode(m_ctx, m_pkt, m_frame);
            if (ret < 0) {
                // 单包解不出：跳过这一段，继续处理后面的包（规格 4.3 条目 11）。
                // 整段停会让一秒干扰毁掉后面十分钟。
                if (!skipFailedPacket(ret)) {
                    LOG_ERROR(LOG_TAG, "decode failed: %d", ret);
                    goto err;
                }
            } else {
                m_consecutiveFailures = 0;
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

int AudioDecode::Impl::decodePackets(const std::vector<AudioBuffer::AudioBufferPtr> &packets)
{
    if (packets.empty()) {
        return 0;
    }

    if (m_parserCtx) {
        size_t totalSize = 0;
        for (const auto &packet : packets) {
            if (packet) {
                totalSize += packet->size();
            }
        }
        if (totalSize == 0) {
            return 0;
        }

        std::vector<char> joined(totalSize);
        size_t offset = 0;
        for (const auto &packet : packets) {
            if (!packet || packet->size() == 0) {
                continue;
            }
            std::memcpy(joined.data() + offset, packet->data(), packet->size());
            offset += packet->size();
        }
        return decode(joined.data(), static_cast<ssize_t>(offset));
    }

    for (const auto &packet : packets) {
        if (!packet || packet->size() == 0) {
            continue;
        }

        m_pkt->data = reinterpret_cast<uint8_t *>(packet->data());
        m_pkt->size = static_cast<int>(packet->size());
        int ret     = decode(m_ctx, m_pkt, m_frame);
        if (ret < 0) {
            if (skipFailedPacket(ret)) {
                continue;
            }
            LOG_ERROR(LOG_TAG, "decode packet failed: %s", getAVErrorString(ret));
            m_pkt->data = nullptr;
            m_pkt->size = 0;
            return ret;
        }
        m_consecutiveFailures = 0;
    }

    m_pkt->data = nullptr;
    m_pkt->size = 0;
    int ret     = decode(m_ctx, m_pkt, m_frame);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "decode packet flush failed: %s", getAVErrorString(ret));
        return ret;
    }
    return 0;
}

int AudioDecode::Impl::decodePacket(const AudioBuffer::AudioBufferPtr &packet)
{
    if (!packet || packet->size() == 0) {
        return 0;
    }

    // ffmpeg 的输入要求尾部零填充；包工厂的包是精确帧长，这里补一份。
    // m_scratch 复用，容量只随最大包变化（O(1)），不做每包分配。
    const size_t size = packet->size();
    m_scratch.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(m_scratch.data(), packet->data(), size);
    std::memset(m_scratch.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    m_pkt->data = m_scratch.data();
    m_pkt->size = static_cast<int>(size);
    int ret     = decode(m_ctx, m_pkt, m_frame);
    m_pkt->data = nullptr;
    m_pkt->size = 0;
    if (ret < 0) {
        // 单包坏：记一次跳过并继续（规格 4.1：坏帧之后还有数据）。
        if (skipFailedPacket(ret)) {
            return 0;
        }
        LOG_ERROR(LOG_TAG, "decode packet failed: %s", getAVErrorString(ret));
        return ret;
    }
    m_consecutiveFailures = 0;
    return 0;
}

int AudioDecode::Impl::flushDecode()
{
    m_pkt->data = nullptr;
    m_pkt->size = 0;
    int ret     = decode(m_ctx, m_pkt, m_frame);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "decode flush failed: %s", getAVErrorString(ret));
        return ret;
    }
    return 0;
}

char *AudioDecode::Impl::getAVErrorString(int errnum)
{
    av_strerror(errnum, m_error, sizeof(m_error));
    return m_error;
}

bool AudioDecode::Impl::skipFailedPacket(int err)
{
    if (!isSkippablePacketError(err)) {
        return false;
    }
    ++m_skippedPackets;
    ++m_consecutiveFailures;
    if (m_consecutiveFailures >= kMaxConsecutiveFailedPackets) {
        LOG_ERROR(LOG_TAG,
                  "giving up after %d consecutive undecodable packets (skipped %d in total, last: %s)",
                  m_consecutiveFailures, m_skippedPackets, getAVErrorString(err));
        return false;
    }
    LOG_WARNING(LOG_TAG, "skipping undecodable packet #%d in a row: %s", m_consecutiveFailures,
                getAVErrorString(err));
    if (m_callback != nullptr) {
        m_callback->onAudioDecodeSkip();
    }
    return true;
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
    : m_impl(new Impl(codec, callback, AudioCodecConfig()))
{
}

AudioDecode::AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback,
                         const AudioCodecConfig &config)
    : m_impl(new Impl(codec, callback, config))
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

int AudioDecode::decodePackets(const std::vector<AudioBuffer::AudioBufferPtr> &packets)
{
    return m_impl->decodePackets(packets);
}

int AudioDecode::decodePacket(const AudioBuffer::AudioBufferPtr &packet)
{
    return m_impl->decodePacket(packet);
}

int AudioDecode::flushDecode()
{
    return m_impl->flushDecode();
}

int AudioDecode::skippedPacketCount() const
{
    return m_impl->skippedPacketCount();
}
