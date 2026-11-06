#include "MKVExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <cstring>

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

MKVExtractor::~MKVExtractor() { }

MKVPacketReader *MKVExtractor::reader()
{
    // 解复用面在播放期（解码线程第一次拉包时）创建：init 期的 demux 在扫描结束后已经关掉了。
    if (m_reader == nullptr && m_dataSource != nullptr && m_fileSize > 0) {
        m_reader.reset(new MKVPacketReader(m_dataSource, m_fileSize));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr MKVExtractor::packetAt(size_t index)
{
    MKVPacketReader *packetReader = reader();
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

bool MKVExtractor::seekPacketByte(int64_t offset)
{
    MKVPacketReader *packetReader = reader();
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

int64_t MKVExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

status_t MKVExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }
    m_fileSize = fileSize;

    // init 期的 demux 只服务这一次**只读扫描**，结束时整个关掉（ADR 0003：只留 O(1) 摘要）。
    MKVPacketReader scanner(m_dataSource, fileSize);
    if (!scanner.open()) {
        LOGW("initWithFFmpegDemux demux open failed");
        return NO_INIT;
    }

    MKVStreamInfo info;
    if (!scanner.streamInfo(info)) {
        LOGW("initWithFFmpegDemux invalid audio stream");
        return NO_INIT;
    }

    m_audioCodecID = static_cast<AudioCodecID>(info.codecId);
    m_audioSpec.sampleRate = info.sampleRate;
    m_audioSpec.numChannel = info.channels;
    m_audioSpec.bitsPerSample
        = info.bitsPerCodedSample > 0 ? info.bitsPerCodedSample : info.bitsPerRawSample;
    m_audioSpec.bytesPerSample
        = m_audioSpec.bitsPerSample > 0 ? (m_audioSpec.bitsPerSample + 7) / 8 : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
    m_bitRate          = info.bitRate;
    m_blockAlign       = info.blockAlign;
    if (!info.extradata.empty()) {
        m_codecExtraData = std::make_shared<AudioBuffer>(info.extradata.size());
        memcpy(m_codecExtraData->data(), info.extradata.data(), info.extradata.size());
    }

    // 只读扫描：逐块走完整份文件，只为按**包时间轴**累加标称时长，逐块丢掉负载。
    // 时长归属（#15）：以「逐包时长累加」为准，容器声明值只作兜底。Matroska 的
    // Segment/Duration 是封装器写进去的，写法出错的工具真实存在——music-mkv.mkv 就把
    // 393216 个采样当毫秒写成 393.216 s，实际只有 24.576 s。包时长来自解复用出的时间轴，
    // 不受该字段污染；累加成本已经付过（本就在读文件）。
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

    // 容器声明的时长（fmt->duration，单位微秒）只作兜底：它与包时间轴一致时才用得上。
    const int64_t declaredUs = scanner.declaredDurationUs();

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
