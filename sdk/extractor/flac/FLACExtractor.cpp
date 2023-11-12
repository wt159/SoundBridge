#include "FLACExtractor.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "FLACExtractor"

using namespace sdk_utils;

FLACExtractor::FLACExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_metaBuf(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
{
    m_initCheck = init();
}

FLACExtractor::~FLACExtractor() { }

status_t FLACExtractor::init()
{
    //(1)　判断文件格式
    uint8_t id[4];
    int offset = 0;
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
        memcpy(m_header.fileMark.id, id, 4);

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

            // if (block.header.type >= PICTURE) {
            //     LOGE("invalid type");
            //     offset -= 4;
            //     break;
            // }

            int size = block.header.blockSize;
            if (block.header.type == STREAMINFO) {
                LOGD("block header type is STREAMINFO");
                uint8_t data[size];
            if (m_dataSource->readAt(offset, data, size) < size) {
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
                block.data.streamInfo.totalSamples = ((data[13] & 0x0f) << 32)
                    | ((data[14] & 0xff) << 24) | ((data[15] & 0xff) << 16)
                    | ((data[16] & 0xff) << 8) | (data[17] & 0xff);
                memcpy(block.data.streamInfo.md5, &data[18], 16);
                LOGD("minBlockSize: %d, maxBlockSize: %d, minFrameSize: %d, maxFrameSize: %d, "
                     "sampleRate: %d, channels: %d, bitsPerSample: %d, totalSamples: %d",
                     block.data.streamInfo.minBlockSize, block.data.streamInfo.maxBlockSize,
                     block.data.streamInfo.minFrameSize, block.data.streamInfo.maxFrameSize,
                     block.data.streamInfo.sampleRate, block.data.streamInfo.channels,
                     block.data.streamInfo.bitsPerSample, block.data.streamInfo.totalSamples);
                m_header.metaDataBlockVec.push_back(block);
                m_spec.sampleRate     = block.data.streamInfo.sampleRate;
                m_spec.numChannel     = block.data.streamInfo.channels;
                m_spec.bitsPerSample  = block.data.streamInfo.bitsPerSample;
                m_spec.bytesPerSample = m_spec.bitsPerSample >> 3;
                m_spec.format         = getAudioFormatByBitPreSample(m_spec.bitsPerSample);
                m_spec.samples        = block.data.streamInfo.totalSamples;
                m_spec.durationMs     = ((uint64_t)m_spec.samples * 1000) / m_spec.sampleRate;
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
            m_header.metaDataBlockVec.push_back(block);

            if (block.header.isLastBlock) {
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
        frame.header.syncCode = ((temp[0] & 0xff) << 6) | ((temp[1] & 0xFC) >> 2);
        frame.header.blockingStrategy = (temp[1] & 0x01);
        frame.header.bsCode = (temp[2] & 0xf0) >> 4;
        frame.header.srCode = (temp[2] & 0x0F);
        frame.header.chMode = (temp[3] & 0xf0) >> 4;
        frame.header.bpsCode = (temp[3] & 0x0e) >> 1;
        LOGI("syncCode: %d, blockingStrategy: %d, bs_code: %d, sr_code: %d, chMode: %d, bpsCode: %d",
             frame.header.syncCode, frame.header.blockingStrategy, frame.header.bsCode, frame.header.srCode,
             frame.header.chMode, frame.header.bpsCode);
    }

    if (!m_validFormat) {
        return NO_INIT;
    }
    m_audioCodecID = AUDIO_CODEC_ID_FLAC;

    ssize_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    fileSize -= offset;
    m_metaBuf = std::make_shared<AudioBuffer>(fileSize);
    if (m_dataSource->readAt(offset, m_metaBuf->data(), fileSize) < fileSize) {
        LOGE("readAt failed");
        return NO_MEMORY;
    }

    LOGI("sampleRate: %d, numChannel: %d, bytesPerSample: %d, samples:%d", m_spec.sampleRate, m_spec.numChannel,
         m_spec.bytesPerSample, m_spec.samples);
    LOGI("durationMs: %lu", m_spec.durationMs);
    return NO_ERROR;
}
