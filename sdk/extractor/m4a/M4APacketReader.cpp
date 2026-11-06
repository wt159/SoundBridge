#include "M4APacketReader.h"
#include "LogWrapper.h"
#include <chrono>
#include <climits>
#include <cstring>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/rational.h"
}

#define LOG_TAG "M4APacketReader"

namespace {
// AVIO 缓冲：判据公式里的固定项（64 KB），与素材时长无关。
const int kIoBufferSize = 64 * 1024;
} // namespace

M4AStreamInfo::M4AStreamInfo()
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

M4APacketReader::M4APacketReader(DataSourceBase *source, off64_t fileSize)
    : m_ioData()
    , m_fmt(nullptr)
    , m_avio(nullptr)
    , m_pkt(nullptr)
    , m_audioIndex(-1)
    , m_lastPacketDuration(0)
    , m_boundaryMicros(0)
    , m_useAdts(false)
    , m_adtsProfile(1)
    , m_adtsSampleRateIndex(0)
    , m_adtsChannelConfig(0)
{
    m_ioData.source = source;
    m_ioData.pos    = 0;
    m_ioData.size   = static_cast<int64_t>(fileSize);
}

M4APacketReader::~M4APacketReader()
{
    close();
}

int M4APacketReader::avioRead(void *opaque, uint8_t *buf, int buf_size)
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

int64_t M4APacketReader::avioSeek(void *opaque, int64_t offset, int whence)
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

bool M4APacketReader::open()
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
                                &M4APacketReader::avioRead, nullptr,
                                &M4APacketReader::avioSeek);
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
    prepareAdts();
    return true;
}

void M4APacketReader::close()
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

bool M4APacketReader::streamInfo(M4AStreamInfo &info) const
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

bool M4APacketReader::nextPacket(AudioBuffer::AudioBufferPtr &packet)
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
    const size_t payload = static_cast<size_t>(m_pkt->size);
    const size_t prefix  = m_useAdts ? 7u : 0u;
    AudioBuffer::AudioBufferPtr out
        = std::make_shared<AudioBuffer>(payload + prefix);
    if (m_useAdts) {
        uint8_t adts[7];
        buildAdtsHeader(adts, m_adtsProfile, m_adtsSampleRateIndex, m_adtsChannelConfig,
                        static_cast<int>(payload + prefix));
        std::memcpy(out->data(), adts, sizeof(adts));
    }
    std::memcpy(out->data() + prefix, m_pkt->data, payload);
    av_packet_unref(m_pkt);
    packet = out;
    return true;
}

bool M4APacketReader::seekTo(off64_t offset)
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

off64_t M4APacketReader::position() const
{
    if (m_fmt == nullptr || m_fmt->pb == nullptr) {
        return 0;
    }
    return static_cast<off64_t>(avio_tell(m_fmt->pb));
}

int64_t M4APacketReader::declaredDurationUs() const
{
    return m_fmt != nullptr ? m_fmt->duration : 0;
}


int M4APacketReader::sampleRateToIndex(int sampleRate)
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

bool M4APacketReader::parseAudioSpecificConfig(const uint8_t *data, int size,
                                              int &audioObjectType, int &sampleRateIndex,
                                              int &channelConfig)
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

void M4APacketReader::buildAdtsHeader(uint8_t *out, int profile, int sampleRateIndex,
                                      int channelConfig, int frameLen)
{
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

void M4APacketReader::prepareAdts()
{
    m_useAdts              = false;
    m_adtsProfile          = 1;
    m_adtsSampleRateIndex  = 0;
    m_adtsChannelConfig    = 0;
    if (m_fmt == nullptr || m_audioIndex < 0
        || m_audioIndex >= static_cast<int>(m_fmt->nb_streams)) {
        return;
    }
    AVStream *audioStream = m_fmt->streams[m_audioIndex];
    if (audioStream == nullptr || audioStream->codecpar == nullptr) {
        return;
    }
    AVCodecParameters *par = audioStream->codecpar;
    if (par->codec_id != AV_CODEC_ID_AAC) {
        return; // ALAC 没有 parser，边界由提取器直接给，不做 ADTS 包装
    }

    int aot     = 2;
    int srIndex = sampleRateToIndex(par->sample_rate);
    int chCfg   = par->channels;
    if (par->extradata != nullptr && par->extradata_size > 0) {
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
    if (srIndex < 0) {
        return;
    }

    m_useAdts             = true;
    int profile           = aot - 1;
    if (aot == 5 || aot == 29) {
        profile = 1;
    }
    m_adtsProfile         = profile;
    m_adtsSampleRateIndex = srIndex;
    m_adtsChannelConfig   = chCfg;
}
