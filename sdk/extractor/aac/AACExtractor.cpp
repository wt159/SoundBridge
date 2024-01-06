#include "AACExtractor.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "AACExtractor"

using namespace sdk_utils;

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
    , m_metaBuf(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
{
    m_initCheck = init();
}

AACExtractor::~AACExtractor()
{
}

status_t AACExtractor::init()
{
    //(1)　判断文件格式，确定为ADIF或ADTS
    uint32_t id;
    if (m_dataSource->readAt(0, &id, sizeof id) < 2) {
        return NO_INIT;
    }
    if (memcmp(&id, "ADIF", 4) == 0) {
        LOG_INFO(LOG_TAG, "this is ADIF format");
        LOG_ERROR(LOG_TAG, "ADIF format is not supported");
        return NO_INIT;
    } else {
        LOG_INFO(LOG_TAG, "this is ADTS format");
        uint8_t data[10];
        if (m_dataSource->readAt(0, data, 10) < 10) {
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
        m_spec.sampleRate         = getSampleRate(m_header.adts.samplingFrequencyIndex);
        m_spec.numChannel         = m_header.adts.channelConfiguration;
    }
    m_audioCodecID = AUDIO_CODEC_ID_AAC;
    
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    m_metaBuf = std::make_shared<AudioBuffer>(fileSize);
    if (m_dataSource->readAt(0, m_metaBuf->data(), fileSize) < fileSize) {
        LOG_ERROR(LOG_TAG, "readAt failed");
        return NO_MEMORY;
    }

    LOG_INFO(LOG_TAG, "sampleRate: %d, numChannel: %d, bytesPerSample: %d", m_spec.sampleRate,
             m_spec.numChannel, m_spec.bytesPerSample);
    return NO_ERROR;
}
