#include "AIFFExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "AIFFExtractor"

using namespace sdk_utils;
using namespace AIFF;

AIFFExtractor::AIFFExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
{
    m_initCheck = init();
}

AIFFExtractor::~AIFFExtractor() { }

status_t AIFFExtractor::init()
{
    off64_t offset = 0, fileSize = 0;
    int invalidChunkCount = 0;
    m_dataSource->getSize(&fileSize);
    while (1) {
        if (offset >= fileSize) {
            break;
        }
        uint8_t chunkHeader[8];
        if (m_dataSource->readAt(offset, chunkHeader, sizeof(chunkHeader))
            < (ssize_t)sizeof(chunkHeader)) {
            m_validFormat = false;
            break;
        }
        offset += sizeof(chunkHeader);
        Chunk ck;
        ck.chunkID   = U32_AT(&chunkHeader[0]);
        ck.chunkSize = U32_AT(&chunkHeader[4]);
        LOGI("chunkID=%#x, chunkSize=%d", ck.chunkID, ck.chunkSize);

        if (ck.chunkID == kChunkID_FORM) {
            uint8_t formType[4];
            if (m_dataSource->readAt(offset, formType, sizeof(formType))
                < (ssize_t)sizeof(formType)) {
                m_validFormat = false;
                break;
            }
            m_formChunk.chunkID    = ck.chunkID;
            m_formChunk.chunkSize  = ck.chunkSize;
            m_formChunk.formType   = U32_AT(&formType[0]);
            offset                += sizeof(formType);
            if (memcmp(formType, "AIFF", 4) == 0) {
                m_validFormat = true;
            } else if (memcmp(formType, "AIFC", 4) == 0) {
                m_validFormat = true;
            } else {
                m_validFormat = false;
                break;
            }

        } else if (ck.chunkID == kChunkID_COMM) {
            uint8_t temp[18];
            if (m_dataSource->readAt(offset, temp, sizeof(temp)) < (ssize_t)sizeof(temp)) {
                m_validFormat = false;
                break;
            }
            offset += sizeof(temp);
            std::shared_ptr<CommonChunk> cChunk(new CommonChunk());
            cChunk->chunkID         = ck.chunkID;
            cChunk->chunkSize       = ck.chunkSize;
            cChunk->numChannels     = U16_AT(&temp[0]);
            cChunk->numSampleFrames = U32_AT(&temp[2]);
            cChunk->sampleSize      = U16_AT(&temp[6]);
            cChunk->sampleRate = (uint32_t)ieee754_80bits_to_long_double(&temp[8]);
            m_formChunk.chunkVec.push_back(cChunk);
            LOGI("numChannels=%u", cChunk->numChannels);
            LOGI("numSampleFrames=%u", cChunk->numSampleFrames);
            LOGI("sampleSize=%u", cChunk->sampleSize);
            LOGI("sampleRate=%u", cChunk->sampleRate);
            commonChunk2AudioSpec(cChunk.get(), m_audioSpec);
            offset += ck.chunkSize - sizeof(temp);
        } else if (ck.chunkID == kChunkID_FVER) {
            uint8_t temp[4];
            if (m_dataSource->readAt(offset, temp, sizeof(temp)) < (ssize_t)sizeof(temp)) {
                m_validFormat = false;
                break;
            }
            offset += sizeof(temp);
            std::shared_ptr<FormatVersionChunk> fvChunk(new FormatVersionChunk());
            fvChunk->chunkID   = ck.chunkID;
            fvChunk->chunkSize = ck.chunkSize;
            fvChunk->timestamp = U32_AT(&temp[0]);
            m_formChunk.chunkVec.push_back(fvChunk);
            LOGI("timestamp=%u", fvChunk->timestamp);
        } else if (ck.chunkID == kChunkID_SSND) {
            uint8_t temp[8];
            if (m_dataSource->readAt(offset, temp, sizeof(temp)) < (ssize_t)sizeof(temp)) {
                m_validFormat = false;
                break;
            }
            offset += sizeof(temp);
            std::shared_ptr<SoundDataChunk> sdChunk(new SoundDataChunk());
            sdChunk->chunkID   = ck.chunkID;
            sdChunk->chunkSize = ck.chunkSize;
            sdChunk->offset    = U32_AT(&temp[0]);
            sdChunk->blockSize = U32_AT(&temp[4]);
            m_formChunk.chunkVec.push_back(sdChunk);
            LOGI("offset=%u", sdChunk->offset);
            LOGI("blockSize=%u", sdChunk->blockSize);
            size_t soundDataSize = ck.chunkSize - sizeof(temp);
            sdChunk->soundData   = std::make_shared<AudioBuffer>(soundDataSize);
            if (m_dataSource->readAt(offset, sdChunk->soundData->data(), soundDataSize)
                < (ssize_t)soundDataSize) {
                m_validFormat = false;
                break;
            }
            offset    += soundDataSize;
            m_metaBuf  = sdChunk->soundData;
        } else {
            if (invalidChunkCount++ > 15) {
                m_validFormat = false;
                break;
            }
        }
    }

    if (!m_validFormat) {
        return NO_INIT;
    }

    m_audioCodecID = AUDIO_CODEC_ID_NONE;

    return OK;
}

void AIFFExtractor::commonChunk2AudioSpec(const AIFF::CommonChunk *cChunk, AudioSpec &audioSpec)
{
    audioSpec.numChannel     = cChunk->numChannels;
    audioSpec.sampleRate     = cChunk->sampleRate;
    audioSpec.bitsPerSample  = cChunk->sampleSize;
    audioSpec.bytesPerSample = cChunk->sampleSize >> 3;
    audioSpec.format         = getAudioFormatByBitPreSample(cChunk->sampleSize);
    audioSpec.samples        = cChunk->numSampleFrames;
    audioSpec.durationMs     = (uint64_t)cChunk->numSampleFrames * 1000 / cChunk->sampleRate;
    LOGI("durationMs=%llu", audioSpec.durationMs);
}
