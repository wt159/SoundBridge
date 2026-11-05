#include "FLACExtractor.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "FLACExtractor"

using namespace sdk_utils;
static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool FLACExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[4] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    return memcmp(buf, "fLaC", 4) == 0;
}

FLACExtractor::FLACExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_audioDataOffset(0)
    , m_dataSize(0)
    , m_maxFrameBytes(0)
    , m_reader()
    , m_nextPacketIndex(0)
{
    m_initCheck = init();
}

FLACExtractor::~FLACExtractor() { }

FLACFrameReader *FLACExtractor::reader()
{
    if (m_reader == nullptr && m_dataSource != nullptr) {
        m_reader.reset(new FLACFrameReader(m_dataSource, m_audioDataOffset, m_dataSize,
                                           m_maxFrameBytes));
        m_nextPacketIndex = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr FLACExtractor::packetAt(size_t index)
{
    FLACFrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return nullptr;
    }
    // 只支持顺序拉取：游标 rewind（index 回到 0）等价于从流头重来，其余跳跃视为结束。
    if (index != m_nextPacketIndex) {
        if (index != 0) {
            return nullptr;
        }
        frameReader->seekTo(0);
        m_nextPacketIndex = 0;
    }

    AudioBuffer::AudioBufferPtr packet;
    if (!frameReader->nextPacket(packet)) {
        return nullptr;
    }
    ++m_nextPacketIndex;
    return packet;
}

bool FLACExtractor::seekPacketByte(off64_t offset)
{
    FLACFrameReader *frameReader = reader();
    if (frameReader == nullptr) {
        return false;
    }
    frameReader->seekTo(offset);
    m_nextPacketIndex = 0;
    return true;
}

off64_t FLACExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

status_t FLACExtractor::init()
{
    // Check file format
    uint8_t id[4];
    off64_t offset = 0;
    if (m_dataSource->readAt(0, &id, 4) < 4) {
        return NO_INIT;
    }
    offset += 4;
    if (memcmp(&id, "fLaC", 4) != 0) {
        LOGE(LOG_TAG, "this is invalid format");
        return NO_INIT;
    } else {
        LOG_INFO(LOG_TAG, "this is FLAC format");
        m_validFormat = true;

        // init 只读扫描：走完元数据块只为拿到 STREAMINFO 与音频数据起点，
        // 不留任何随块数 / 包数增长的结构（ADR 0003：只留 O(1) 定长摘要）。
        while (1) {
            LOGD("offset: %d", offset);
            uint8_t header[4];
            if (m_dataSource->readAt(offset, header, 4) < 4) {
                return NO_INIT;
            }
            offset += 4;

            MetaDataBlock block;
            block.header.isLastBlock = (header[0] >> 7) & 0x01;
            block.header.type        = header[0] & 0x7F;
            block.header.blockSize
                = ((header[1] & 0xff) << 16) | ((header[2] & 0xff) << 8) | (header[3] & 0xff);

            LOGI("isLastBlock: %d, type: %d, blockSize: %d", block.header.isLastBlock,
                 block.header.type, block.header.blockSize);

            int size = block.header.blockSize;
            if (block.header.type == STREAMINFO) {
                LOGD("block header type is STREAMINFO");
                // STREAMINFO 恒为 34 字节（规格 10.#2）。这里不按声明长度开栈上变长数组：
                // blockSize 上限 0xFFFFFF = 16MB，超过默认栈就是栈溢出；声明长度不等于 34
                // 本身也说明块头已经坏了，直接判失败。
                const size_t kStreamInfoSize = 34;
                if (size != static_cast<int>(kStreamInfoSize)) {
                    LOGE("invalid STREAMINFO block size: %d", size);
                    return NO_INIT;
                }
                std::vector<uint8_t> data(kStreamInfoSize);
                if (m_dataSource->readAt(offset, data.data(), data.size())
                    < static_cast<ssize_t>(data.size())) {
                    return NO_INIT;
                }
                block.data.streamInfo.minBlockSize = ((data[0] & 0xff) << 8) | (data[1] & 0xff);
                block.data.streamInfo.maxBlockSize = ((data[2] & 0xff) << 8) | (data[3] & 0xff);
                block.data.streamInfo.minFrameSize
                    = ((data[4] & 0xff) << 16) | ((data[5] & 0xff) << 8) | (data[6] & 0xff);
                block.data.streamInfo.maxFrameSize
                    = ((data[7] & 0xff) << 16) | ((data[8] & 0xff) << 8) | (data[9] & 0xff);
                block.data.streamInfo.sampleRate = ((data[10] & 0xff) << 12)
                    | ((data[11] & 0xff) << 4) | ((data[12] & 0xff) >> 4);
                block.data.streamInfo.channels = ((data[12] & 0x0e) >> 1) + 1;
                block.data.streamInfo.bitsPerSample
                    = (((data[12] & 0x01) << 4) | ((data[13] & 0xff) >> 4)) + 1;
                block.data.streamInfo.totalSamples = ((uint64_t)(data[13] & 0x0f) << 32)
                    | ((data[14] & 0xff) << 24) | ((data[15] & 0xff) << 16)
                    | ((data[16] & 0xff) << 8) | (data[17] & 0xff);
                memcpy(block.data.streamInfo.md5, &data[18], 16);
                LOGD("minBlockSize: %d, maxBlockSize: %d, minFrameSize: %d, maxFrameSize: %d, "
                     "sampleRate: %d, channels: %d, bitsPerSample: %d, totalSamples: %d",
                     block.data.streamInfo.minBlockSize, block.data.streamInfo.maxBlockSize,
                     block.data.streamInfo.minFrameSize, block.data.streamInfo.maxFrameSize,
                     block.data.streamInfo.sampleRate, block.data.streamInfo.channels,
                     block.data.streamInfo.bitsPerSample, block.data.streamInfo.totalSamples);
                m_maxFrameBytes      = block.data.streamInfo.maxFrameSize;
                m_spec.sampleRate     = block.data.streamInfo.sampleRate;
                m_spec.numChannel     = block.data.streamInfo.channels;
                m_spec.bitsPerSample  = block.data.streamInfo.bitsPerSample;
                m_spec.bytesPerSample = m_spec.bitsPerSample >> 3;
                m_spec.format         = getAudioFormatByBitPreSample(m_spec.bitsPerSample);
                m_spec.samples        = block.data.streamInfo.totalSamples;
                // sampleRate > 0 是硬不变量（规格 5.4 H2）：只有它成立才做除法，
                // 否则 durationMs 保持 0 并且这份头判为坏，不许把 sr=0 带出去。
                m_spec.durationMs = 0;
                if (m_spec.sampleRate > 0) {
                    m_spec.durationMs = ((uint64_t)m_spec.samples * 1000) / m_spec.sampleRate;
                } else {
                    LOGE("invalid sampleRate 0 in STREAMINFO");
                    return NO_INIT;
                }
            } else if (block.header.type == PADDING) {
                LOGD("block header type is PADDING");
            } else if (block.header.type == APPLICATION) {
                LOGD("block header type is APPLICATION");
            } else if (block.header.type == SEEKTABLE) {
                LOGD("block header type is SEEKTABLE");
            } else if (block.header.type == VORBIS_COMMENT) {
                LOGD("block header type is VORBIS_COMMENT");
            } else if (block.header.type == CUESHEET) {
                LOGD("block header type is CUESHEET");
            } else if (block.header.type == PICTURE) {
                LOGD("block header type is PICTURE");
            } else {
                LOGD("block header type is UNKNOWN");
            }
            offset += size;

            if (block.header.isLastBlock) {
                m_audioDataOffset = offset;
                break;
            }
        }
    }
    {
        // Audio Frame
        uint8_t temp[4];
        if (m_dataSource->readAt(offset, temp, 4) < 4) {
            return NO_INIT;
        }
        AudioFrame frame;
        frame.header.syncCode         = ((temp[0] & 0xff) << 6) | ((temp[1] & 0xFC) >> 2);
        frame.header.blockingStrategy = (temp[1] & 0x01);
        frame.header.bsCode           = (temp[2] & 0xf0) >> 4;
        frame.header.srCode           = (temp[2] & 0x0F);
        frame.header.chMode           = (temp[3] & 0xf0) >> 4;
        frame.header.bpsCode          = (temp[3] & 0x0e) >> 1;
        LOGI(
            "syncCode: %d, blockingStrategy: %d, bs_code: %d, sr_code: %d, chMode: %d, bpsCode: %d",
            frame.header.syncCode, frame.header.blockingStrategy, frame.header.bsCode,
            frame.header.srCode, frame.header.chMode, frame.header.bpsCode);
    }

    if (!m_validFormat) {
        return NO_INIT;
    }
    m_audioCodecID = AUDIO_CODEC_ID_FLAC;

    off64_t fileSize = 0;
    static_cast<DataSource *>(m_dataSource)->getSize(&fileSize);
    m_dataSize = fileSize;

    LOGI("sampleRate: %d, numChannel: %d, bytesPerSample: %d, samples:%d", m_spec.sampleRate,
         m_spec.numChannel, m_spec.bytesPerSample, m_spec.samples);
    LOGI("durationMs: %lu", m_spec.durationMs);
    normalizeAudioSpec(m_spec);
    return NO_ERROR;
}
