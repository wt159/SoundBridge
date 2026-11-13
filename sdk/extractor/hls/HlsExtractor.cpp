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
    , m_lastReadError(0)
    , m_diagValid(false)
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
    // 诊断旁路生命周期不变量 1/3（#151 D13）：open 时置「无失败」，
    // 失败状态不得跨越一次 open。
    m_lastReadError = 0;
    m_diagValid = false;

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
    // P3 读超时（#155 D15）：播放主路径唯一墙钟。三条实现约束：① 只设 rw_timeout
    // 不设 timeout——后者经 tcp.c:139-141 静默覆盖前者，且不在 hls 的 7 项抄写里
    // （实测 HLS+timeout 仍 15 s 挂死）；② rw_timeout 经 avio.c 的 av_opt_copy(parent)
    // 下传 tcp、也在 hls.c 的 7 项里抄给 segment，一处设置同时覆盖 playlist 与 segment；
    // ③ 沿用既有 3000000 µs（与 fetchHead / HttpSource socket 同口径），VOD/LIVE 都补、
    // 不做形态分支；3 s 到点的表现形态是 EOF / 结束序列，不是可上报的超时错误。
    av_dict_set(&opts, "rw_timeout", "3000000", 0);
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

    // 非音频包（元数据等）只跳不缓存；上限防坏流在非音频包上原地打转。
    // 上限耗尽按 nullptr 停止：PacketCursor 在 kUnknownPacketCount 下的 nullptr
    // 语义是「序列结束」而非「跳过」，解码侧随后走既有 EOS 汇总兜底
    // （ADR 0002：可解性不在提取器判定），不再重跑跳包循环。
    int ret = 0;
    bool gotAudio = false;
    for (int skipped = 0; skipped < kNonAudioPacketSkipLimit; ++skipped) {
        ret = av_read_frame(m_fmt, m_pkt);
        if (ret < 0) {
            // 包耗尽与坏数据都走这里：可解性不在提取器判定（ADR 0002），
            // 交给播放期的「包耗尽 + EOS 汇总」兜底。
            // P3 诊断旁路（#151 D11/D13）：负返回记入诊断；nullptr 语义不扩展，
            // PacketCursor 与 4 个 PacketReader 零改动。VOD/LIVE 都记，
            // 只由消费侧分流（D14：durationMs==0 的 EOF = 供给停滞）。
            m_lastReadError = ret;
            m_diagValid = true;
            return nullptr;
        }
        if (m_pkt->stream_index == m_audioIndex && m_pkt->size > 0) {
            gotAudio = true;
            break;
        }
        av_packet_unref(m_pkt);
    }
    if (!gotAudio) {
        return nullptr;
    }

    const size_t payload = static_cast<size_t>(m_pkt->size);
    AudioBuffer::AudioBufferPtr out = std::make_shared<AudioBuffer>(payload);
    std::memcpy(out->data(), m_pkt->data, payload);
    av_packet_unref(m_pkt);
    ++m_consumed;
    // 诊断旁路生命周期不变量 2/3（#151 D13）：任一成功读包即清零——失败状态
    // 不得跨越一次成功读包。
    m_lastReadError = 0;
    m_diagValid = false;
    return out;
}

bool HlsExtractor::consumeLastReadError(int *outError)
{
    // 诊断旁路生命周期不变量 3/3（#151 D13）：读取一次即失效——不得被消费两次。
    if (outError != nullptr) *outError = m_lastReadError;
    if (!m_diagValid) {
        return false;
    }
    m_diagValid = false;
    return true;
}

bool HlsExtractor::reopenConnection()
{
    // P3 无状态重开原语（#149 / ADR 0007）：closeFmt + open，决策（预算/等待/重试）
    // 全在解码线程状态机。closeFmt 释放 m_pkt 与 m_fmt；open 重算 m_audioIndex /
    // m_audioSpec / extradata 并重新分配 m_pkt。
    // m_consumed 必须归零：重开是从「新滑动窗口的头」重新顺序迭代，不是 seek——
    // 不复位会让 packetAt(0) 的 index==0 分支走 av_seek_frame（LIVE 必失败 →
    // nullptr → 解码侧立刻 EOS，#149 裁决 6 的坑）。
    closeFmt();
    m_consumed = 0;
    return open() == OK;
}
