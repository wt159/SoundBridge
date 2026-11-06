#include "AIFFExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <algorithm>
#include <cstring>

#define LOG_TAG "AIFFExtractor"

using namespace sdk_utils;
using namespace AIFF;
static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool AIFFExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[12] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    if (memcmp(buf, "FORM", 4) != 0) {
        return false;
    }
    return (memcmp(buf + 8, "AIFF", 4) == 0) || (memcmp(buf + 8, "AIFC", 4) == 0);
}

AIFFExtractor::AIFFExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_dataTruncated(false)
    , m_audioSpec()
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_dataOffset(0)
    , m_dataSize(0)
    , m_reader()
    , m_nextPacketIndex(0)
{
    m_initCheck = init();
}

AIFFExtractor::~AIFFExtractor() { }

PcmPacketReader *AIFFExtractor::reader()
{
    if (m_reader == nullptr && m_dataSource != nullptr) {
        const size_t frameBytes = static_cast<size_t>(m_audioSpec.numChannel)
            * static_cast<size_t>(m_audioSpec.bytesPerSample);
        m_reader.reset(new PcmPacketReader(m_dataSource, m_dataOffset,
                                           static_cast<off64_t>(m_dataSize), frameBytes));
        m_nextPacketIndex = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr AIFFExtractor::packetAt(size_t index)
{
    PcmPacketReader *packetReader = reader();
    if (packetReader == nullptr) {
        return nullptr;
    }
    // 只支持顺序拉取。index == 0 是「本段包序列的第一包」：回到 SSND 起点重来
    //（新任务与顺序重放 seek 都从这里起）。其余跳跃不接受。
    if (index == 0) {
        packetReader->rewind();
        m_nextPacketIndex = 0;
    } else if (index != m_nextPacketIndex) {
        return nullptr;
    }

    AudioBuffer::AudioBufferPtr packet;
    if (!packetReader->nextPacket(packet)) {
        return nullptr;
    }
    // AIFF 的 PCM 是大端：逐包在交出之前换成主机字节序（旧形式是整段换一次）。
    AudioBuffer2HostEndian(packet, m_audioSpec);
    ++m_nextPacketIndex;
    return packet;
}

int64_t AIFFExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

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
            offset += sizeof(formType);
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
            cChunk->sampleRate      = (uint32_t)ieee754_80bits_to_long_double(&temp[8]);
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
            // FVER 只对 AIFC 有意义，读出来只为日志：不保留 chunk 表（#61 的 O(1) 摘要）。
            LOGI("timestamp=%u", U32_AT(&temp[0]));
        } else if (ck.chunkID == kChunkID_SSND) {
            uint8_t temp[8];
            if (m_dataSource->readAt(offset, temp, sizeof(temp)) < (ssize_t)sizeof(temp)) {
                m_validFormat = false;
                break;
            }
            offset += sizeof(temp);
            LOGI("offset=%u", U32_AT(&temp[0]));
            LOGI("blockSize=%u", U32_AT(&temp[4]));

            // 规格 T2 / 条目 6：与 wav 同一条规则——按 min(声明长度, 文件剩余) 读。
            // durationMs 仍由 COMM 的 numSampleFrames 得出（标称值不变）。
            uint64_t declaredSize
                = (ck.chunkSize > sizeof(temp)) ? ck.chunkSize - sizeof(temp) : 0;
            uint64_t available
                = (static_cast<uint64_t>(fileSize) > static_cast<uint64_t>(offset))
                ? static_cast<uint64_t>(fileSize) - static_cast<uint64_t>(offset)
                : 0;
            m_dataTruncated      = declaredSize > available;
            size_t soundDataSize = static_cast<size_t>(std::min(declaredSize, available));
            LOGI("soundDataSize=%u (declared %llu%s)", (unsigned)soundDataSize,
                 (unsigned long long)declaredSize, m_dataTruncated ? ", truncated" : "");
            if (soundDataSize == 0) {
                m_validFormat = false;
                break;
            }
            // payload 不再在 init 期整首装载、也不整段换字节序（ADR 0003：只留 O(1) 摘要）：
            // 切片与换字节序都发生在播放期，由 packetAt() 逐包完成。
            m_dataOffset      = offset;
            m_dataSize        = soundDataSize;
            offset           += soundDataSize;
            // 可播样本按实际读到的算（规格 T2），标称时长留在 durationMs 里
            if (m_audioSpec.numChannel > 0 && m_audioSpec.bytesPerSample > 0) {
                size_t frames
                    = soundDataSize / (m_audioSpec.numChannel * m_audioSpec.bytesPerSample);
                if (frames < m_audioSpec.samples) {
                    m_audioSpec.samples = frames;
                }
            }
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
    normalizeAudioSpec(m_audioSpec);
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

void AIFFExtractor::AudioBuffer2HostEndian(AudioBuffer::AudioBufferPtr &audioBuf,
                                           const AudioSpec &spec)
{
    // AIFF is big endian
    if (!is_little_endian()) {
        return;
    }

    for (size_t i = 0; i < audioBuf->size(); i += spec.bytesPerSample) {
        if (spec.bytesPerSample == 2) {
            uint16_t *p = (uint16_t *)(audioBuf->data() + i);
            *p          = U16_AT((uint8_t *)(audioBuf->data() + i));
        } else if (spec.bytesPerSample == 4) {
            uint32_t *p = (uint32_t *)(audioBuf->data() + i);
            *p          = U32_AT((uint8_t *)(audioBuf->data() + i));
        } else if (spec.bytesPerSample == 8) {
            uint64_t *p = (uint64_t *)(audioBuf->data() + i);
            *p          = U64_AT((uint8_t *)(audioBuf->data() + i));
        }
    }
}
