#include "Errors.h"
#include "LogWrapper.h"
#include "WAVExtractor.h"
#include <cstring>

#define LOG_TAG                        "WAVExtractor"

#define CHANNEL_MASK_USE_CHANNEL_ORDER 0

enum {
    WAVE_FORMAT_PCM        = 0x0001,
    WAVE_FORMAT_IEEE_FLOAT = 0x0003,
    WAVE_FORMAT_ALAW       = 0x0006,
    WAVE_FORMAT_MULAW      = 0x0007,
    WAVE_FORMAT_MSGSM      = 0x0031,
    WAVE_FORMAT_EXTENSIBLE = 0xFFFE
};

static const char *WAVEEXT_SUBFORMAT   = "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71";
static const char *AMBISONIC_SUBFORMAT = "\x00\x00\x21\x07\xD3\x11\x86\x44\xC8\xC1\xCA\x00\x00\x00";

static uint32_t U32_LE_AT(const uint8_t *ptr)
{
    return ptr[3] << 24 | ptr[2] << 16 | ptr[1] << 8 | ptr[0];
}

static uint16_t U16_LE_AT(const uint8_t *ptr)
{
    return ptr[1] << 8 | ptr[0];
}

WAVExtractor::WAVExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_decode(nullptr)
    , m_validFormat(false)
{
    m_initCheck = init();
}

// void WAVExtractor::readAudioRawData(off64_t offset, size_t size, void *buf)
// {
//     m_dataSource->readAt(m_dataOffset + offset, buf, size);
// }

void WAVExtractor::readAudioRawData(AudioBuffer::AudioBufferPtr &bufPtr) { 
    bufPtr = m_rawBuf;
}

WAVExtractor::~WAVExtractor() { }

status_t WAVExtractor::init()
{
    uint8_t header[12];
    if (m_dataSource->readAt(0, header, sizeof(header)) < (ssize_t)sizeof(header)) {
        return NO_INIT;
    }

    if (memcmp(header, "RIFF", 4) || memcmp(&header[8], "WAVE", 4)) {
        return NO_INIT;
    }

    size_t totalSize = U32_LE_AT(&header[4]);

    off64_t offset       = 12;
    size_t remainingSize = totalSize;
    while (remainingSize >= 8) {
        uint8_t chunkHeader[8];
        if (m_dataSource->readAt(offset, chunkHeader, 8) < 8) {
            return NO_INIT;
        }

        remainingSize -= 8;
        offset        += 8;

        uint32_t chunkSize = U32_LE_AT(&chunkHeader[4]);
        LOG_INFO(LOG_TAG, "chunkSize=%lu", (long unsigned)chunkSize);

        if (chunkSize > remainingSize) {
            return NO_INIT;
        }

        if (!memcmp(chunkHeader, "fmt ", 4)) {
            if (chunkSize < 16) {
                return NO_INIT;
            }

            uint8_t formatSpec[40];
            if (m_dataSource->readAt(offset, formatSpec, 2) < 2) {
                return NO_INIT;
            }

            m_waveFormat = U16_LE_AT(formatSpec);
            if (m_waveFormat != WAVE_FORMAT_PCM && m_waveFormat != WAVE_FORMAT_IEEE_FLOAT
                && m_waveFormat != WAVE_FORMAT_ALAW && m_waveFormat != WAVE_FORMAT_MULAW
                && m_waveFormat != WAVE_FORMAT_MSGSM && m_waveFormat != WAVE_FORMAT_EXTENSIBLE) {
                LOG_ERROR(LOG_TAG, "Unsupported wave format (%d)", m_waveFormat);
                return ERROR_UNSUPPORTED;
            }

            uint8_t fmtSize = 16;
            if (m_waveFormat == WAVE_FORMAT_EXTENSIBLE) {
                fmtSize = 40;
            }
            if (m_dataSource->readAt(offset, formatSpec, fmtSize) < fmtSize) {
                return NO_INIT;
            }

            m_audioSpec.numChannel = m_numChannels = U16_LE_AT(&formatSpec[2]);
            if (m_numChannels < 1 || m_numChannels > 8) {
                LOG_ERROR(LOG_TAG, "Unsupported number of channels (%d)", m_numChannels);
                return ERROR_UNSUPPORTED;
            }
            LOG_INFO(LOG_TAG, "numChannels=%d", m_numChannels);

            if (m_waveFormat != WAVE_FORMAT_EXTENSIBLE) {
                if (m_numChannels != 1 && m_numChannels != 2) {
                    LOG_WARNING(LOG_TAG,
                                "More than 2 channels (%d) in non-WAVE_EXT, unknown channel mask",
                                m_numChannels);
                }
            }

            m_audioSpec.sampleRate = m_sampleRate = U32_LE_AT(&formatSpec[4]);
            LOG_INFO(LOG_TAG, "sampleRate=%d", m_sampleRate);
            if (m_sampleRate == 0) {
                LOG_ERROR(LOG_TAG, "Invalid sample rate (%d)", m_sampleRate);
                return ERROR_MALFORMED;
            }

            m_audioSpec.bitsPerSample = m_bitsPerSample = U16_LE_AT(&formatSpec[14]);
            m_audioSpec.bytesPerSample                  = m_bitsPerSample >> 3;
            m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
            LOG_INFO(LOG_TAG, "bitsPerSample=%d", m_bitsPerSample);
            LOG_INFO(LOG_TAG, "bytesPerSample=%d", m_audioSpec.bytesPerSample);
            LOG_INFO(LOG_TAG, "format=%d", m_audioSpec.format);
            if (m_waveFormat == WAVE_FORMAT_EXTENSIBLE) {
                uint16_t validBitsPerSample = U16_LE_AT(&formatSpec[18]);
                if (validBitsPerSample != m_bitsPerSample) {
                    if (validBitsPerSample != 0) {
                        LOG_ERROR(LOG_TAG, "validBits(%d) != bitsPerSample(%d) are not supported",
                                  validBitsPerSample, m_bitsPerSample);
                        return ERROR_UNSUPPORTED;
                    } else {
                        LOG_WARNING(LOG_TAG, "WAVE_EXT has 0 valid bits per sample, ignoring");
                    }
                }

                m_channelMask = U32_LE_AT(&formatSpec[20]);
                LOG_DEBUG(LOG_TAG, "numChannels=%d channelMask=0x%x", m_numChannels, m_channelMask);
                if ((m_channelMask >> 18) != 0) {
                    LOG_ERROR(LOG_TAG, "invalid channel mask 0x%x", m_channelMask);
                    return ERROR_MALFORMED;
                }

                if ((m_channelMask != CHANNEL_MASK_USE_CHANNEL_ORDER)
                    && (m_channelMask != m_numChannels)) {
                    LOG_ERROR(LOG_TAG, "invalid number of channels (%d) in channel mask (0x%x)",
                              m_channelMask, m_channelMask);
                    return ERROR_MALFORMED;
                }

                m_waveFormat = U16_LE_AT(&formatSpec[24]);
                if (memcmp(&formatSpec[26], WAVEEXT_SUBFORMAT, 14)
                    && memcmp(&formatSpec[26], AMBISONIC_SUBFORMAT, 14)) {
                    LOG_ERROR(LOG_TAG, "unsupported GUID");
                    return ERROR_UNSUPPORTED;
                }
            } else if (m_waveFormat == WAVE_FORMAT_PCM) {
                if (m_bitsPerSample != 8 && m_bitsPerSample != 16 && m_bitsPerSample != 24
                    && m_bitsPerSample != 32) {
                    return ERROR_UNSUPPORTED;
                }
            } else if (m_waveFormat == WAVE_FORMAT_IEEE_FLOAT) {
                if (m_bitsPerSample != 32) { // TODO we don't support double
                    return ERROR_UNSUPPORTED;
                }
            } else if (m_waveFormat == WAVE_FORMAT_MSGSM) {
                if (m_bitsPerSample != 0) {
                    return ERROR_UNSUPPORTED;
                }
            } else if (m_waveFormat == WAVE_FORMAT_MULAW || m_waveFormat == WAVE_FORMAT_ALAW) {
                if (m_bitsPerSample != 8) {
                    return ERROR_UNSUPPORTED;
                }
            } else {
                return ERROR_UNSUPPORTED;
            }

            m_validFormat = true;
        } else if (!memcmp(chunkHeader, "data", 4)) {
            if (m_validFormat) {
                m_dataOffset = offset;
                m_dataSize   = chunkSize;
                LOG_INFO(LOG_TAG, "dataOffset=%lu, dataSize=%lu",
                         (long unsigned)m_dataOffset, ( long unsigned)m_dataSize);

                if (m_waveFormat == WAVE_FORMAT_MSGSM) {
                    // 65 bytes decode to 320 8kHz samples
                    m_durationMs = 1000000LL * (m_dataSize / 65 * 320) / 8000;
                } else {
                    if (!m_audioSpec.bytesPerSample || !m_numChannels)
                        return ERROR_MALFORMED;

                    size_t num_samples  = m_dataSize / (m_numChannels * m_audioSpec.bytesPerSample);
                    m_audioSpec.samples = num_samples;
                    if (!m_sampleRate)
                        return ERROR_MALFORMED;

                    m_durationMs = 1000LL * num_samples / m_sampleRate;
                    LOG_INFO(LOG_TAG, "durationMs = %lu", (long unsigned)m_durationMs);
                }

                m_rawBuf = AudioBuffer::AudioBufferPtr(new AudioBuffer(m_dataSize));
                if (m_dataSource->readAt(m_dataOffset, m_rawBuf->data(), m_dataSize) < (ssize_t)m_dataSize) {
                    LOG_ERROR(LOG_TAG, "readAt failed");
                    return NO_INIT;
                }
                return OK;
            }
        }
        offset += chunkSize;
    }

    return NO_INIT;
}
