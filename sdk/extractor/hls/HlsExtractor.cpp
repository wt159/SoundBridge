#include "HlsExtractor.h"
#include "AbortInterrupt.hpp"
#include "ErrorUtils.h"
#include "LogWrapper.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/dict.h"
#include "libavutil/rational.h"
}

#include <cstring>

#define LOG_TAG "HlsExtractor"

using namespace sdk_utils;

namespace {

// hls 相对 URI 解析后逐 segment 顺序拉取；非音频包（元数据等）只跳不缓存。
// 上限只防坏流在非音频包上原地打转，正常流远达不到。
const int kNonAudioPacketSkipLimit = 64;

// codec 映射口径与既有枚举一致（AudioCodecID 的取值即 AVCodecID）；
// PCM 等未压缩编码走 AUDIO_CODEC_ID_NONE（同 WAV 提取器：无压缩数据不经解码器）。
AudioCodecID mapAudioCodecId(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC:
        return AUDIO_CODEC_ID_AAC;
    case AV_CODEC_ID_MP3:
        return AUDIO_CODEC_ID_MP3;
    default:
        return AUDIO_CODEC_ID_NONE;
    }
}

} // namespace

HlsExtractor::HlsExtractor(DataSourceBase *source)
    : m_source(source)
    , m_fmt(nullptr)
    , m_pkt(nullptr)
    , m_audioIndex(-1)
    , m_consumed(0)
    , m_initCheck(NO_INIT)
    , m_audioSpec()
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_codecExtraData(nullptr)
    , m_declaredDurationUs(0)
{
    m_initCheck = open();
}

HlsExtractor::~HlsExtractor()
{
    closeFmt();
}

void HlsExtractor::closeFmt()
{
    if (m_pkt != nullptr) {
        av_packet_free(&m_pkt);
    }
    if (m_fmt != nullptr) {
        avformat_close_input(&m_fmt);
    }
    m_audioIndex = -1;
}

status_t HlsExtractor::open()
{
    char uri[2048] = { 0 };
    if (m_source == nullptr || !m_source->getUri(uri, sizeof(uri)) || uri[0] == '\0') {
        // 无 URI 的数据源（本地文件等）不进直通路径：initCheck 落 NO_INIT，
        // 工厂出口收 nullptr，语义保守（#143 契约 §6 的分派失败出口）。
        LOGW("hls open rejected: data source has no uri");
        return NO_INIT;
    }
    m_url = uri;

    avformat_network_init();   // 幂等：原生 URL 层的网络协议初始化

    m_fmt = avformat_alloc_context();
    if (m_fmt == nullptr) {
        LOGW("avformat_alloc_context failed");
        return NO_INIT;
    }

    // 取消贯通（#143 契约 §3/§4）：同一 m_abortDecode 信号经 UrlSource 转发到
    // avformat_open_input / av_read_frame 的阻塞点，不新建信号。
    m_fmt->interrupt_callback.callback = &sbAbortInterruptCallback;
    m_fmt->interrupt_callback.opaque   = m_source;

    AVDictionary *opts = nullptr;
    // 契约 #143 §1 定稿：protocol_whitelist = "http,tcp"，不含 file（恶意清单的
    // 本地盘读取通道）、https/tls（两端不对称 + https 入口 P0 已拒）、crypto（判据外）。
    av_dict_set(&opts, "protocol_whitelist", "http,tcp", 0);
    // 直通装配：不传自定义 pb；s->url 非空 → segment 经 s->io_open 以同白名单重开。
    const int ret = avformat_open_input(&m_fmt, m_url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOGW("avformat_open_input failed: %d", ret);
        // open_input 失败时自己释放了 m_fmt，把口径归一成 closeFmt 能处理的状态。
        m_fmt = nullptr;
        return NO_INIT;
    }

    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        LOGW("avformat_find_stream_info failed");
        closeFmt();
        return NO_INIT;
    }

    m_audioIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audioIndex < 0) {
        LOGW("no audio stream");
        closeFmt();
        return NO_INIT;
    }

    AVStream *stream = m_fmt->streams[m_audioIndex];
    AVCodecParameters *par = stream->codecpar;
    m_audioSpec.sampleRate = par->sample_rate;
    m_audioSpec.numChannel = par->channels;
    m_audioSpec.bitsPerSample
        = par->bits_per_coded_sample > 0 ? par->bits_per_coded_sample : par->bits_per_raw_sample;
    m_audioSpec.bytesPerSample
        = m_audioSpec.bitsPerSample > 0 ? (m_audioSpec.bitsPerSample + 7) / 8 : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);

    // 契约 #143 §5：VOD 由 hls demuxer 提供 duration（容器声明值，微秒）；
    // declaredDurationUs 口径 = AVFormatContext::duration（同 M4APacketReader）。
    m_declaredDurationUs = m_fmt->duration > 0 ? m_fmt->duration : 0;
    if (m_declaredDurationUs > 0) {
        m_audioSpec.durationMs = static_cast<uint64_t>(m_declaredDurationUs / 1000);
    }

    m_audioCodecID = mapAudioCodecId(par->codec_id);
    if (par->extradata != nullptr && par->extradata_size > 0) {
        m_codecExtraData = std::make_shared<AudioBuffer>(
            static_cast<size_t>(par->extradata_size));
        std::memcpy(m_codecExtraData->data(), par->extradata,
                    static_cast<size_t>(par->extradata_size));
    }

    m_pkt = av_packet_alloc();
    if (m_pkt == nullptr) {
        LOGW("av_packet_alloc failed");
        closeFmt();
        return NO_INIT;
    }

    LOGI("hls open ok, codec=%#x, durationUs=%lld", m_audioCodecID,
         (long long)m_declaredDurationUs);
    normalizeAudioSpec(m_audioSpec);
    return OK;
}

AudioBuffer::AudioBufferPtr HlsExtractor::packetAt(size_t index)
{
    if (m_fmt == nullptr || m_pkt == nullptr) {
        return nullptr;
    }
    // 顺序拉取契约（ADR 0003）。index == 0 是「本段包序列的第一包」：
    // 已消费过则回到流头重新迭代；其余跳跃不接受。
    if (index == 0) {
        if (m_consumed > 0) {
            if (av_seek_frame(m_fmt, m_audioIndex, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                LOGW("rewind to stream head failed");
                return nullptr;
            }
        }
        m_consumed = 0;
    } else if (index != m_consumed) {
        return nullptr;
    }

    int ret = 0;
    for (int skipped = 0; skipped < kNonAudioPacketSkipLimit; ++skipped) {
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            // 包耗尽与坏数据都走这里：可解性不在提取器判定（ADR 0002），
            // 交给播放期的「包耗尽 + EOS 汇总」兜底。
            return nullptr;
        }
        if (m_pkt->stream_index == m_audioIndex && m_pkt->size > 0) {
            break;
        }
        av_packet_unref(m_pkt);
    }
    if (ret < 0) {
        return nullptr;
    }

    const size_t payload = static_cast<size_t>(m_pkt->size);
    AudioBuffer::AudioBufferPtr out = std::make_shared<AudioBuffer>(payload);
    std::memcpy(out->data(), m_pkt->data, payload);
    av_packet_unref(m_pkt);
    ++m_consumed;
    return out;
}

status_t HlsExtractor::seekToMs(uint64_t targetMs, uint64_t *actualMs)
{
    if (m_fmt == nullptr || m_audioIndex < 0) {
        return NO_INIT;
    }
    // VOD 有界窗口（#143 契约 §5）：hls demuxer 提供 duration → 时间轴 seek；
    // 目标超出可解内容的处置归调用方（m_durationMs 兜底），这里只尽力定位。
    const int64_t targetUs = static_cast<int64_t>(targetMs) * 1000;
    if (av_seek_frame(m_fmt, m_audioIndex, targetUs, AVSEEK_FLAG_BACKWARD) < 0) {
        LOGW("hls seek to %llums failed", (unsigned long long)targetMs);
        return INVALID_OPERATION;
    }
    if (actualMs != nullptr) {
        *actualMs = targetMs;
    }
    return OK;
}
