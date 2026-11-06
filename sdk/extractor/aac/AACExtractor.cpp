#include "AACExtractor.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "AACExtractor"

using namespace sdk_utils;

// ID3v2 前缀长度：0 = 无前缀；声明合法时返回 10 + tagSize；声明超出整份文件时
// 只认 10 字节头（与 mp3 的 clamp 规则一致，规格 10.#19）。size 是整份文件可用
// 字节数，data 只需前 10 字节有效。init / sniff / countAdtsFrames 共用这一份逻辑。
static size_t id3PrefixSize(const uint8_t *data, size_t size)
{
    if (data == nullptr || size < 10 || memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    const size_t tagSize = ((data[6] & 0x7f) << 21) | ((data[7] & 0x7f) << 14)
        | ((data[8] & 0x7f) << 7) | (data[9] & 0x7f);
    const size_t prefix = tagSize + 10;
    return (prefix > size) ? 10 : prefix;
}

static bool readHeader(DataSourceBase *source, off64_t offset, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(offset, buf, size) >= static_cast<ssize_t>(size);
}

bool AACExtractor::sniff(DataSourceBase *source)
{
    uint8_t probe[10] = { 0 };
    off64_t fileSize  = 0;
    off64_t offset    = 0;
    if (source != nullptr && source->getSize(&fileSize) == OK && fileSize >= 10
        && source->readAt(0, probe, 10) == 10) {
        offset = static_cast<off64_t>(id3PrefixSize(probe, static_cast<size_t>(fileSize)));
    }
    uint8_t buf[4] = { 0 };
    if (!readHeader(source, offset, buf, sizeof(buf))) {
        return false;
    }
    if (memcmp(buf, "ADIF", 4) == 0) {
        return true;
    }
    return (buf[0] == 0xFF && (buf[1] & 0xF0) == 0xF0);
}

// Returns the sample rate based on the sampling frequency index
uint32_t getSampleRate(const uint8_t sfIndex)
{
    static const uint32_t sampleRates[]
        = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };

    if (sfIndex < sizeof(sampleRates) / sizeof(sampleRates[0])) {
        return sampleRates[sfIndex];
    }

    return 0;
}

AACExtractor::AACExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_metaBuf(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_spec()
    , m_header()
    , m_dataSize(0)
    , m_audioStart(0)
    , m_reader()
    , m_nextPacketIndex(0)
    , m_sequenceOffset(0)
{
    m_initCheck = init();
}

AACExtractor::~AACExtractor() { }

status_t AACExtractor::init()
{
    // 先算出 ID3v2 前缀，与 sniff() / countAdtsFrames() 共用同一份逻辑。
    // 旧实现从 offset 0 解 ADTS 头，带 200 字节前缀的合法文件会被解析成垃圾。
    // m_metaBuf 仍读整份文件（payload 含 tag，规格 7.1），只挪动「解析头」的位置。
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    uint8_t probe[10] = { 0 };
    const off64_t prefix = (m_dataSource->readAt(0, probe, 10) == 10)
        ? static_cast<off64_t>(id3PrefixSize(probe, static_cast<size_t>(fileSize)))
        : 0;

    // Check file format: ADIF or ADTS
    uint32_t id;
    if (m_dataSource->readAt(prefix, &id, sizeof id) < 2) {
        return NO_INIT;
    }
    if (memcmp(&id, "ADIF", 4) == 0) {
        LOG_INFO(LOG_TAG, "this is ADIF format");
        LOG_ERROR(LOG_TAG, "ADIF format is not supported");
        return NO_INIT;
    } else {
        LOG_INFO(LOG_TAG, "this is ADTS format");
        uint8_t data[10];
        if (m_dataSource->readAt(prefix, data, 10) < 10) {
            return NO_INIT;
        }
        m_header.adts.syncWord               = (data[0] << 4) | (data[1] >> 4);
        m_header.adts.id                     = (data[1] >> 3) & 0x01;
        m_header.adts.layer                  = (data[1] >> 1) & 0x03;
        m_header.adts.protectionAbsent       = data[1] & 0x01;
        m_header.adts.profile                = (data[2] >> 6) & 0x03;
        m_header.adts.samplingFrequencyIndex = (data[2] >> 2) & 0x0F;
        m_header.adts.privateBit             = (data[2] >> 1) & 0x01;
        m_header.adts.channelConfiguration
            = (((data[2] & 0x01) << 2) | ((data[3] & 0xc0) >> 6)) & 0x07;
        m_header.adts.originalCopy     = (data[3] >> 5) & 0x01;
        m_header.adts.home             = (data[3] >> 4) & 0x01;
        m_header.adts.copyrightIDBit   = (data[3] >> 3) & 0x01;
        m_header.adts.copyrightIDStart = (data[3] >> 2) & 0x01;
        m_header.adts.frameLength
            = ((data[3] & 0x03) << 11) | ((data[4] & 0xff) << 3) | ((data[5] & 0xe0) >> 5);
        m_header.adts.bufferFullness   = ((data[5] & 0x1F) << 6) | ((data[6] & 0xfc) >> 2);
        m_header.adts.numRawDataBlocks = data[6] & 0x03;
        // ADTS 头必须自洽（规格 10.#4）：syncWord 固定 0xFFF，channelConfiguration 为 0 表示
        // 「声道配置在带内」，ADTS 流里拿不到，等于没有声道数。旧代码两项都不校验，一份全零头
        // 会被解析成 sr=96000 ch=0 并且 return NO_ERROR，垃圾文件就这样进了播放列表。
        if (m_header.adts.syncWord != 0x0FFF) {
            LOGE("invalid ADTS syncWord: %#x", m_header.adts.syncWord);
            return NO_INIT;
        }
        if (m_header.adts.channelConfiguration == 0) {
            LOGE("invalid ADTS channelConfiguration: 0");
            return NO_INIT;
        }
        m_spec.sampleRate              = getSampleRate(m_header.adts.samplingFrequencyIndex);
        m_spec.numChannel              = m_header.adts.channelConfiguration;
        if (m_spec.sampleRate == 0) {
            LOGE("invalid ADTS samplingFrequencyIndex: %d", m_header.adts.samplingFrequencyIndex);
            return NO_INIT;
        }
    }
    m_audioCodecID = AUDIO_CODEC_ID_AAC;

    // init 只读扫描（ADR 0003）：只数 ADTS 帧拿标称时长，不留 payload 与帧表。
    // 「音频」从 ID3 前缀之后开始，标签不进 payload（规格 7.3 M6）：若把前缀喂给
    // 解码器，它会被当成不可解帧跳过，EOS 汇总就会误报一条 DataCorrupted。
    if (fileSize <= prefix) {
        return NO_INIT;
    }
    m_dataSize   = fileSize;
    m_audioStart = prefix;
    m_sequenceOffset = prefix;

    m_spec.bitsPerSample  = 16;
    m_spec.format         = getAudioFormatByBitPreSample(m_spec.bitsPerSample);
    m_spec.bytesPerSample = 2;

    const size_t frameCount = AACFrameReader::scanFrameCount(m_dataSource, prefix, fileSize);
    if (frameCount > 0 && m_spec.sampleRate > 0) {
        size_t samplesPerFrame = (static_cast<size_t>(m_header.adts.numRawDataBlocks) + 1) * 1024;
        int64_t durationUs     = static_cast<int64_t>(frameCount)
            * static_cast<int64_t>(samplesPerFrame) * 1000000LL
            / static_cast<int64_t>(m_spec.sampleRate);
        if (durationUs > 0) {
            m_spec.durationMs = static_cast<uint64_t>(durationUs / 1000);
        }
    }

    LOG_INFO(LOG_TAG, "sampleRate: %d, numChannel: %d, bytesPerSample: %d", m_spec.sampleRate,
             m_spec.numChannel, m_spec.bytesPerSample);
    normalizeAudioSpec(m_spec);
    return NO_ERROR;
}

AACFrameReader *AACExtractor::reader()
{
    if (m_reader == nullptr && m_dataSource != nullptr) {
        m_reader.reset(new AACFrameReader(m_dataSource, m_audioStart, m_dataSize));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = m_audioStart;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr AACExtractor::packetAt(size_t index)
{
    // 解复用面在播放期（解码线程第一次拉包时）创建，init 期只有 O(1) 摘要。
    AACFrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return nullptr;
    }
    if (index == 0) {
        frameReader->seekTo(m_sequenceOffset);
        m_nextPacketIndex = 0;
    } else if (index != m_nextPacketIndex) {
        return nullptr;
    }

    AudioBuffer::AudioBufferPtr packet;
    if (!frameReader->nextPacket(packet)) {
        return nullptr;
    }
    ++m_nextPacketIndex;
    return packet;
}

bool AACExtractor::seekPacketByte(int64_t offset)
{
    AACFrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return false;
    }
    // 0 在调用方语境里是「流头」；aac 的流头是 ID3v2 前缀之后的第一个 ADTS 帧。
    if (offset < m_audioStart) {
        offset = m_audioStart;
    }
    frameReader->seekTo(offset);
    m_sequenceOffset  = offset;
    m_nextPacketIndex = 0;
    return true;
}

int64_t AACExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

AudioBuffer::AudioBufferPtr AACExtractor::getMetaData()
{
    // 整块视图（测试量测仪器专用，生产链零调用）：按需读取音频负载，语义与改造前一致。
    if (m_metaBuf == nullptr && m_dataSource != nullptr && m_dataSize > m_audioStart) {
        const off64_t length = m_dataSize - m_audioStart;
        AudioBuffer::AudioBufferPtr buffer
            = std::make_shared<AudioBuffer>(static_cast<size_t>(length));
        if (m_dataSource->readAt(m_audioStart, buffer->data(), static_cast<size_t>(length))
            == static_cast<ssize_t>(length)) {
            m_metaBuf = buffer;
        } else {
            LOG_ERROR(LOG_TAG, "readAt failed for the legacy whole view");
        }
    }
    return m_metaBuf;
}
