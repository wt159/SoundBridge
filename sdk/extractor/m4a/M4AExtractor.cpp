#include "M4AExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "M4AExtractor"

using namespace sdk_utils;

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

M4AExtractor::~M4AExtractor() { }

M4APacketReader *M4AExtractor::reader()
{
    // 解复用面在播放期（解码线程第一次拉包时）创建：init 期的 demux 在扫描结束后已经关掉了。
    if (m_reader == nullptr && m_dataSource != nullptr && m_fileSize > 0) {
        m_reader.reset(new M4APacketReader(m_dataSource, m_fileSize));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr M4AExtractor::packetAt(size_t index)
{
    M4APacketReader *packetReader = reader();
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

bool M4AExtractor::seekPacketByte(int64_t offset)
{
    M4APacketReader *packetReader = reader();
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

int64_t M4AExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

status_t M4AExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }
    m_fileSize = fileSize;

    // init 期的 demux 只服务这一次**只读扫描**，结束时整个关掉（ADR 0003：只留 O(1) 摘要）。
    M4APacketReader scanner(m_dataSource, fileSize);
    if (!scanner.open()) {
        LOGW("initWithFFmpegDemux demux open failed");
        return NO_INIT;
    }

    M4AStreamInfo info;
    if (!scanner.streamInfo(info)) {
        LOGW("initWithFFmpegDemux invalid audio stream");
        return NO_INIT;
    }

    m_audioCodecID         = static_cast<AudioCodecID>(info.codecId);
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

    // 只读扫描：逐包走完整份文件，只为按**包时间轴**累加标称时长，逐包丢掉负载。
    // 时长归属（#15 / #39）：以逐包 duration 累加为准，mdhd/mvhd 的声明值只作兜底；
    // 包时长来自 stts 解出的时间轴，不受容器脏字段污染。moov 即便在尾部也只是被读一遍，
    // 不留下任何随包数增长的索引 / 逐包时间轴 / seek 表（ADR 0003「读过 ≠ 常驻」）。
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
