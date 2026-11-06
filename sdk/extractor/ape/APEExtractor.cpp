#include "APEExtractor.h"
#include "ByteUtils.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <climits>
#include <cstring>
#include <vector>
extern "C" {
#include "libavutil/avutil.h"
}

#define LOG_TAG "APEExtractor"

using namespace sdk_utils;

// APE version 3.98+ uses the descriptor/header layout we parse here.
static const uint16_t kApeMinVersion     = 3980;
// APE descriptor is 52 bytes for v3.98+.
static const size_t kApeDescriptorBytes  = 52;
// APE header is at least 24 bytes (fixed portion) and can include optional fields.
static const uint32_t kApeHeaderMinBytes = 24;
// Safety cap to avoid reading unreasonable header sizes from a corrupt file.
static const uint32_t kApeHeaderMaxBytes = 1024;

static bool parseApeHeader(DataSourceBase *source, int &sampleRate, int &channels,
                           int &bitsPerSample)
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
    if (descriptorBytes < sizeof(desc) || headerBytes < kApeHeaderMinBytes
        || headerBytes > kApeHeaderMaxBytes) {
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
    uint8_t buf[4] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    return memcmp(buf, "MAC ", 4) == 0;
}

APEExtractor::APEExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_codecExtraData(nullptr)
    , m_audioSpec()
    , m_bitRate(0)
    , m_blockAlign(0)
    , m_fileSize(0)
    , m_audioPacketCount(0)
    , m_reader()
    , m_nextPacketIndex(0)
    , m_sequenceOffset(0)
{
    m_initCheck = initWithFFmpegDemux();
}

APEExtractor::~APEExtractor() { }

APEPacketReader *APEExtractor::reader()
{
    // 解复用面在播放期（解码线程第一次拉包时）创建：init 期的 demux 在扫描结束后已经关掉了。
    if (m_reader == nullptr && m_dataSource != nullptr && m_fileSize > 0) {
        m_reader.reset(new APEPacketReader(m_dataSource, m_fileSize));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr APEExtractor::packetAt(size_t index)
{
    APEPacketReader *packetReader = reader();
    if (packetReader == nullptr) {
        return nullptr;
    }
    // 只支持顺序拉取。index == 0 是「本段包序列的第一包」：回到本段起点重来
    // （新任务从流头开始，顺序重放 seek 之后同样回到流头）。其余跳跃不接受。
    if (index == 0) {
        if (!packetReader->seekTo(m_sequenceOffset)) {
            return nullptr;
        }
        m_nextPacketIndex = 0;
    } else if (index != m_nextPacketIndex) {
        return nullptr;
    }

    AudioBuffer::AudioBufferPtr packet;
    if (!packetReader->nextPacket(packet)) {
        return nullptr;
    }
    ++m_nextPacketIndex;
    return packet;
}

bool APEExtractor::seekPacketByte(int64_t offset)
{
    APEPacketReader *packetReader = reader();
    if (packetReader == nullptr) {
        return false;
    }
    if (!packetReader->seekTo(offset)) {
        return false;
    }
    m_sequenceOffset  = offset;
    m_nextPacketIndex = 0;
    return true;
}

int64_t APEExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

status_t APEExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }
    m_fileSize = fileSize;

    // init 期的 demux 只服务这一次**只读扫描**，结束时整个关掉（ADR 0003：只留 O(1) 摘要）。
    APEPacketReader scanner(m_dataSource, fileSize);
    if (!scanner.open()) {
        LOGW("initWithFFmpegDemux demux open failed");
        return NO_INIT;
    }

    APEStreamInfo info;
    if (!scanner.streamInfo(info)) {
        LOGW("initWithFFmpegDemux invalid audio stream");
        return NO_INIT;
    }

    m_audioCodecID         = static_cast<AudioCodecID>(info.codecId);
    m_audioSpec.sampleRate = info.sampleRate;
    m_audioSpec.numChannel = info.channels;
    m_audioSpec.bitsPerSample
        = info.bitsPerCodedSample > 0 ? info.bitsPerCodedSample : info.bitsPerRawSample;
    // spec 兜底与迁移前一致：ffmpeg 对部分 ape 给不出完整 spec 时退回手写 APE 头解析，
    // 这条只补 sampleRate / channels / bits，不参与时长来源。
    if (m_audioSpec.bitsPerSample == 0 || m_audioSpec.sampleRate == 0
        || m_audioSpec.numChannel == 0) {
        int parsedSampleRate    = m_audioSpec.sampleRate;
        int parsedChannels      = m_audioSpec.numChannel;
        int parsedBitsPerSample = m_audioSpec.bitsPerSample;
        if (parseApeHeader(m_dataSource, parsedSampleRate, parsedChannels, parsedBitsPerSample)) {
            m_audioSpec.sampleRate    = parsedSampleRate;
            m_audioSpec.numChannel    = parsedChannels;
            m_audioSpec.bitsPerSample = parsedBitsPerSample;
            LOGI("parseApeHeader ok: sr=%d ch=%d bps=%d", m_audioSpec.sampleRate,
                 m_audioSpec.numChannel, m_audioSpec.bitsPerSample);
        } else {
            LOGW("parseApeHeader failed or incomplete, sr=%d ch=%d bps=%d", m_audioSpec.sampleRate,
                 m_audioSpec.numChannel, m_audioSpec.bitsPerSample);
        }
    }
    m_audioSpec.bytesPerSample
        = m_audioSpec.bitsPerSample > 0 ? (m_audioSpec.bitsPerSample + 7) / 8 : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
    m_bitRate          = info.bitRate;
    m_blockAlign       = info.blockAlign;
    if (m_blockAlign == 0 && m_audioSpec.numChannel > 0 && m_audioSpec.bitsPerSample > 0) {
        m_blockAlign = m_audioSpec.numChannel * (m_audioSpec.bitsPerSample / 8);
    }
    if (!info.extradata.empty()) {
        m_codecExtraData = std::make_shared<AudioBuffer>(info.extradata.size());
        memcpy(m_codecExtraData->data(), info.extradata.data(), info.extradata.size());
    }

    // 只读扫描：逐包走完整份文件，只为按**包时间轴**累加标称时长，逐包丢掉负载。
    // 时长归属（#15 / #39）：逐包 duration 累加优先，容器声明只兜底。
    // 实测（ffmpeg 4.4.4）：ape 解复用器给的 pkt->duration **恒为 0**，拿不到独立于
    // 声明字段的时间轴，因此实际总是落到 declaredDurationUs() 兜底——与迁移前口径
    // 完全一致，不改数值。累加逻辑保留：换成会填 duration 的解复用器时自动前移。
    int64_t totalPktDuration = 0;
    AudioBuffer::AudioBufferPtr packet;
    while (scanner.nextPacket(packet)) {
        ++m_audioPacketCount;
        if (scanner.lastPacketDuration() > 0) {
            totalPktDuration += scanner.lastPacketDuration();
        }
        packet.reset();
    }
    const uint64_t measuredMs = (totalPktDuration > 0 && info.timeBaseMs > 0)
        ? static_cast<uint64_t>(totalPktDuration * info.timeBaseMs + 0.5)
        : 0;
    const int64_t declaredUs = scanner.declaredDurationUs();

    // bitRate 兜底与迁移前一致（容器声明时长 + 文件大小估算），必须在 demux 关闭前取值。
    if (m_bitRate == 0 && declaredUs > 0 && fileSize > 0) {
        const double seconds = static_cast<double>(declaredUs) / AV_TIME_BASE;
        if (seconds > 0.0) {
            m_bitRate = static_cast<int>((fileSize * 8.0) / seconds);
        }
    }

    // demux 到此为止：播放期会按需重开一个（规格 4.3 所有权清单）。
    scanner.close();

    if (measuredMs > 0) {
        m_audioSpec.durationMs = measuredMs;
    } else if (declaredUs > 0) {
        m_audioSpec.durationMs = static_cast<uint64_t>(declaredUs / 1000);
    }

    if (m_audioPacketCount == 0) {
        LOGW("initWithFFmpegDemux no audio payload found");
        return NO_INIT;
    }

    LOGI("initWithFFmpegDemux ok, codec=%#x, audioPackets=%zu", m_audioCodecID,
         m_audioPacketCount);
    normalizeAudioSpec(m_audioSpec);
    return OK;
}
