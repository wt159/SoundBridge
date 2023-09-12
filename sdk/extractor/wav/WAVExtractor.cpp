#include "Errors.h"
#include "LogWrapper.h"
#include "WAVExtractor.h"
#include <cstring>

#define LOG_TAG "WAVExtractor"

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

WAVExtractor::WAVExtractor(DataSourceHelper *source)
    : mDataSource(source)
    , mValidFormat(false)
{
    mInitCheck = init();
}

WAVExtractor::~WAVExtractor() { }

size_t WAVExtractor::countTracks()
{
    return mInitCheck == OK ? 1 : 0;
}

status_t WAVExtractor::init()
{
    uint8_t header[12];
    if (mDataSource->readAt(0, header, sizeof(header)) < (ssize_t)sizeof(header)) {
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
        if (mDataSource->readAt(offset, chunkHeader, 8) < 8) {
            return NO_INIT;
        }

        remainingSize -= 8;
        offset += 8;

        uint32_t chunkSize = U32_LE_AT(&chunkHeader[4]);

        if (chunkSize > remainingSize) {
            return NO_INIT;
        }

        if (!memcmp(chunkHeader, "fmt ", 4)) {
            if (chunkSize < 16) {
                return NO_INIT;
            }

            uint8_t formatSpec[40];
            if (mDataSource->readAt(offset, formatSpec, 2) < 2) {
                return NO_INIT;
            }

            mWaveFormat = U16_LE_AT(formatSpec);
            if (mWaveFormat != WAVE_FORMAT_PCM && mWaveFormat != WAVE_FORMAT_IEEE_FLOAT
                && mWaveFormat != WAVE_FORMAT_ALAW && mWaveFormat != WAVE_FORMAT_MULAW
                && mWaveFormat != WAVE_FORMAT_MSGSM && mWaveFormat != WAVE_FORMAT_EXTENSIBLE) {
                return ERROR_UNSUPPORTED;
            }

            uint8_t fmtSize = 16;
            if (mWaveFormat == WAVE_FORMAT_EXTENSIBLE) {
                fmtSize = 40;
            }
            if (mDataSource->readAt(offset, formatSpec, fmtSize) < fmtSize) {
                return NO_INIT;
            }

            mNumChannels = U16_LE_AT(&formatSpec[2]);
            if (mNumChannels < 1 || mNumChannels > 8) {
                LOG_ERROR(LOG_TAG, "Unsupported number of channels (%d)", mNumChannels);
                return ERROR_UNSUPPORTED;
            }

            if (mWaveFormat != WAVE_FORMAT_EXTENSIBLE) {
                if (mNumChannels != 1 && mNumChannels != 2) {
                    LOG_WARNING(LOG_TAG,
                        "More than 2 channels (%d) in non-WAVE_EXT, unknown channel mask",
                        mNumChannels);
                }
            }

            mSampleRate = U32_LE_AT(&formatSpec[4]);

            if (mSampleRate == 0) {
                return ERROR_MALFORMED;
            }

            mBitsPerSample = U16_LE_AT(&formatSpec[14]);

            if (mWaveFormat == WAVE_FORMAT_EXTENSIBLE) {
                uint16_t validBitsPerSample = U16_LE_AT(&formatSpec[18]);
                if (validBitsPerSample != mBitsPerSample) {
                    if (validBitsPerSample != 0) {
                        LOG_ERROR(LOG_TAG, "validBits(%d) != bitsPerSample(%d) are not supported",
                            validBitsPerSample, mBitsPerSample);
                        return ERROR_UNSUPPORTED;
                    } else {
                        LOG_WARNING(LOG_TAG, "WAVE_EXT has 0 valid bits per sample, ignoring");
                    }
                }

                mChannelMask = U32_LE_AT(&formatSpec[20]);
                LOG_DEBUG(LOG_TAG, "numChannels=%d channelMask=0x%x", mNumChannels, mChannelMask);
                if ((mChannelMask >> 18) != 0) {
                    LOG_ERROR(LOG_TAG, "invalid channel mask 0x%x", mChannelMask);
                    return ERROR_MALFORMED;
                }

                if ((mChannelMask != CHANNEL_MASK_USE_CHANNEL_ORDER)
                    && (mChannelMask != mNumChannels)) {
                    LOG_ERROR(LOG_TAG, "invalid number of channels (%d) in channel mask (0x%x)",
                        mChannelMask, mChannelMask);
                    return ERROR_MALFORMED;
                }

                mWaveFormat = U16_LE_AT(&formatSpec[24]);
                if (memcmp(&formatSpec[26], WAVEEXT_SUBFORMAT, 14)
                    && memcmp(&formatSpec[26], AMBISONIC_SUBFORMAT, 14)) {
                    LOG_ERROR(LOG_TAG, "unsupported GUID");
                    return ERROR_UNSUPPORTED;
                }
            } else if (mWaveFormat == WAVE_FORMAT_PCM) {
                if (mBitsPerSample != 8 && mBitsPerSample != 16 && mBitsPerSample != 24
                    && mBitsPerSample != 32) {
                    return ERROR_UNSUPPORTED;
                }
            } else if (mWaveFormat == WAVE_FORMAT_IEEE_FLOAT) {
                if (mBitsPerSample != 32) { // TODO we don't support double
                    return ERROR_UNSUPPORTED;
                }
            } else if (mWaveFormat == WAVE_FORMAT_MSGSM) {
                if (mBitsPerSample != 0) {
                    return ERROR_UNSUPPORTED;
                }
            } else if (mWaveFormat == WAVE_FORMAT_MULAW || mWaveFormat == WAVE_FORMAT_ALAW) {
                if (mBitsPerSample != 8) {
                    return ERROR_UNSUPPORTED;
                }
            } else {
                return ERROR_UNSUPPORTED;
            }

            mValidFormat = true;
        } else if (!memcmp(chunkHeader, "data", 4)) {
            if (mValidFormat) {
                mDataOffset = offset;
                mDataSize   = chunkSize;

                if (mWaveFormat == WAVE_FORMAT_MSGSM) {
                    // 65 bytes decode to 320 8kHz samples
                    mDurationUs = 1000000LL * (mDataSize / 65 * 320) / 8000;
                } else {
                    size_t bytesPerSample = mBitsPerSample >> 3;

                    if (!bytesPerSample || !mNumChannels)
                        return ERROR_MALFORMED;

                    size_t num_samples = mDataSize / (mNumChannels * bytesPerSample);

                    if (!mSampleRate)
                        return ERROR_MALFORMED;

                    mDurationUs = 1000000LL * num_samples / mSampleRate;
                    LOG_INFO(LOG_TAG, "durationUs = %lld", mDurationUs);
                }
                return OK;
            }
        }
        offset += chunkSize;
    }

    return NO_INIT;
}
