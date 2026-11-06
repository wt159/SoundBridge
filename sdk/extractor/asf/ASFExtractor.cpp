#include "ASFExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <climits>
#include <cstdio>
#include <vector>
extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
}
#include <cstring>

#define LOG_TAG "ASFExtractor"

using namespace sdk_utils;
using namespace ASF;
static bool readHeader(DataSourceBase *source, uint8_t *buf, size_t size)
{
    if (source == nullptr || buf == nullptr || size == 0) {
        return false;
    }
    return source->readAt(0, buf, size) >= static_cast<ssize_t>(size);
}

bool ASFExtractor::sniff(DataSourceBase *source)
{
    uint8_t buf[16] = { 0 };
    if (!readHeader(source, buf, sizeof(buf))) {
        return false;
    }
    const uint8_t asfGuid[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                  0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
    for (int i = 0; i < 16; ++i) {
        if (buf[i] != asfGuid[i]) {
            return false;
        }
    }
    return true;
}

constexpr GUID guidHeader = {
    .v1 = 0x75B22630,
    .v2 = 0x668E,
    .v3 = 0x11CF,
    .v4 = { 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C },
};
constexpr GUID guidFileProperties = {
    .v1 = 0x8CABDCA1,
    .v2 = 0xA947,
    .v3 = 0x11CF,
    .v4 = { 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 },
};
constexpr GUID guidStreamProperties = {
    .v1 = 0xB7DC0791,
    .v2 = 0xA9B7,
    .v3 = 0x11CF,
    .v4 = { 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 },
};
constexpr GUID guidStreamAudioMedia = {
    .v1 = 0xF8699E40,
    .v2 = 0x5B4D,
    .v3 = 0x11CF,
    .v4 = { 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B },
};
constexpr GUID guidData = {
    .v1 = 0x75B22636,
    .v2 = 0x668E,
    .v3 = 0x11CF,
    .v4 = { 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C },
};
constexpr GUID guidSimpleIndex = {
    .v1 = 0x33000890,
    .v2 = 0xE5B1,
    .v3 = 0x11CF,
    .v4 = { 0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB },
};

static inline AudioCodecID formatTag2AudioCodecID(int tag)
{
    switch (tag) {
    case 0x161:
        return AUDIO_CODEC_ID_WMAV2;
    case 0x162:
        return AUDIO_CODEC_ID_WMAPRO;
    case 0x163:
        return AUDIO_CODEC_ID_WMALOSSLESS;
    case 0x7A21:
    case 0x7A22:
        return AUDIO_CODEC_ID_GSM;
    default:
        break;
    }
    return AUDIO_CODEC_ID_NONE;
}

static void WAVEFormatEx2AudioSpec(WAVEFormatEx &format, AudioSpec &spec)
{
    spec.sampleRate     = format.samplesPerSec;
    spec.numChannel     = format.channels;
    spec.bitsPerSample  = format.bitsPerSample;
    spec.bytesPerSample = format.bitsPerSample >> 3;
    spec.format         = getAudioFormatByBitPreSample(format.bitsPerSample);
    spec.samples        = 0;
    spec.durationMs     = 0;
}

ASFExtractor::ASFExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_audioStreamNumber(0)
    , m_bitRate(0)
    , m_blockAlign(0)
    , m_codecExtraData(nullptr)
    , m_fileSize(0)
    , m_audioPacketCount(0)
    , m_reader()
    , m_nextPacketIndex(0)
    , m_sequenceOffset(0)
{
    m_initCheck = init();
}

ASFExtractor::~ASFExtractor() { }

status_t ASFExtractor::init()
{
    status_t demuxRet = initWithFFmpegDemux();
    if (demuxRet == OK) {
        return OK;
    }

    status_t ret   = NO_INIT;
    off64_t offset = 0, fileSize = 0;
    m_dataSource->getSize(&fileSize);
    // 兜底解析必须自己保证终止（规格 10.#3）：正常 ASF 的顶层对象只有个位数，
    // 走到上限说明这不是一份能解析的文件，再转下去只会刷日志。
    const size_t kMaxObjectCount = 1024;
    size_t objectCount           = 0;
    while (offset <= fileSize) {
        if (++objectCount > kMaxObjectCount) {
            LOGW("too many objects (%zu), treat as corrupted", objectCount);
            m_validFormat = false;
            break;
        }
        const off64_t objectStart = offset;
        uint8_t objectHeader[24];
        if (m_dataSource->readAt(objectStart, objectHeader, sizeof(objectHeader))
            < (ssize_t)sizeof(objectHeader)) {
            m_validFormat = false;
            break;
        }
        Object obj;
        getGuidObjByArray(objectHeader, obj);
        LOGI("id=%#x, size=%llu", obj.id.v1, obj.size);
        // 对象长度必须至少容纳对象头，且落在文件范围内。否则「下一个对象 = 本对象起点 + 长度」
        // 不再前进（obj.size == 0 时 0 - 24 回绕会原地打转），直接判损坏退出。
        if (obj.size < sizeof(objectHeader)
            || obj.size > static_cast<uint64_t>(fileSize - objectStart)) {
            LOGW("invalid object size %llu at %lld, treat as corrupted", obj.size,
                 (long long)objectStart);
            m_validFormat = false;
            break;
        }
        offset = objectStart + static_cast<off64_t>(sizeof(objectHeader));
        if (obj.id == guidHeader) {
            uint8_t temp[6];
            if (m_dataSource->readAt(offset, temp, sizeof(temp)) < (ssize_t)sizeof(temp)) {
                m_validFormat = false;
                break;
            }
            m_headerObj.id          = obj.id;
            m_headerObj.size        = obj.size;
            m_headerObj.objectCount = U32LE_AT(&temp[0]);
            LOGI("objectCount=%u", m_headerObj.objectCount);
            ret = parseHeaderObject(offset + sizeof(temp), m_headerObj);
            if (ret != OK) {
                m_validFormat = false;
                break;
            }
            m_validFormat = true;
            m_audioCodecID
                = formatTag2AudioCodecID(m_headerObj.audioStreamObj.waveFormatEx.formatTag);
            WAVEFormatEx2AudioSpec(m_headerObj.audioStreamObj.waveFormatEx, m_audioSpec);
        } else if (obj.id == guidData) {
            // TODO: parse data object
            m_dataObj.id   = obj.id;
            m_dataObj.size = obj.size;
            ret            = parseDataObject(offset, m_dataObj);
            if (ret != OK) {
                m_validFormat = false;
                break;
            }
            m_validFormat = true;
        } else if (obj.id == guidSimpleIndex) {
            // TODO: parse simple index object
        } else {
            LOGW("Unknown object, skip it");
        }
        // 严格前进：上面的校验已经保证 obj.size >= 24，off64_t 上不会回绕
        offset = objectStart + static_cast<off64_t>(obj.size);
    }

    if (!m_validFormat) {
        return NO_INIT;
    }

    return OK;
}

status_t ASFExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }
    m_fileSize = fileSize;

    // init 期的 demux 只服务这一次**只读扫描**，结束时整个关掉（ADR 0003：只留 O(1) 摘要）。
    // 尾部 Simple Index Object 由 ffmpeg demux 内部按需读取，自仓不缓存整首索引。
    ASFPacketReader scanner(m_dataSource, fileSize);
    if (!scanner.open()) {
        LOGW("initWithFFmpegDemux demux open failed");
        return NO_INIT;
    }

    ASFStreamInfo info;
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

    // 只读扫描：逐包走完整份文件，按**包时间轴**累加标称时长，逐包丢掉负载。
    // 时长归属（#15 / #39 / bad-input "asf duration follows the packet timeline"）：
    // 逐包 duration 累加优先，File Properties 的 playDuration / 容器 fmt->duration 只兜底；
    // 前者不受容器脏字段污染，改 playDuration ×2 也不改变报出的时长与解出的字节。
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
        const uint64_t declaredMs
            = declaredUs > 0 ? static_cast<uint64_t>(declaredUs / 1000) : 0;
        const uint64_t diff
            = declaredMs > measuredMs ? declaredMs - measuredMs : measuredMs - declaredMs;
        if (declaredMs > 0 && diff * 100 > declaredMs) {
            LOGW("declared duration %llu ms disagrees with packet timeline %llu ms (using packet "
                 "timeline)",
                 static_cast<unsigned long long>(declaredMs),
                 static_cast<unsigned long long>(measuredMs));
        }
    } else if (declaredUs > 0) {
        m_audioSpec.durationMs = static_cast<uint64_t>(declaredUs / 1000);
        LOGW("no packet timeline, fall back to container duration %llu ms",
             static_cast<unsigned long long>(m_audioSpec.durationMs));
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

ASFPacketReader *ASFExtractor::reader()
{
    // 解复用面在播放期（解码线程第一次拉包时）创建：init 期的 demux 在扫描结束后已经关掉了。
    if (m_reader == nullptr && m_dataSource != nullptr && m_fileSize > 0) {
        m_reader.reset(new ASFPacketReader(m_dataSource, m_fileSize));
        m_nextPacketIndex = 0;
        m_sequenceOffset  = 0;
    }
    return m_reader.get();
}

AudioBuffer::AudioBufferPtr ASFExtractor::packetAt(size_t index)
{
    ASFPacketReader *packetReader = reader();
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

bool ASFExtractor::seekPacketByte(int64_t offset)
{
    ASFPacketReader *packetReader = reader();
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

int64_t ASFExtractor::packetBytePosition() const
{
    return m_reader != nullptr ? m_reader->position() : 0;
}

void ASFExtractor::getGuidObjByArray(const uint8_t *array, ASF::Object &obj)
{
    getGuidByArray(&array[0], obj.id);
    obj.size = U64LE_AT(&array[16]);
}
void ASFExtractor::getGuidByArray(const uint8_t *array, ASF::GUID &guid)
{
    guid.v1 = U32LE_AT(&array[0]);
    guid.v2 = U16LE_AT(&array[4]);
    guid.v3 = U16LE_AT(&array[6]);
    memcpy(guid.v4, &array[8], 8);
}
status_t ASFExtractor::parseHeaderObject(off64_t startOffset, ASF::HeaderObject &headerObj)
{
    off64_t offset        = startOffset;
    ssize_t size          = headerObj.size;
    bool foundAudioStream = false;
    while (size > 0) {
        uint8_t objectHeader[24];
        if (m_dataSource->readAt(offset, objectHeader, sizeof(objectHeader))
            < (ssize_t)sizeof(objectHeader)) {
            return NO_INIT;
        }
        Object obj;
        getGuidObjByArray(objectHeader, obj);
        LOGI("id=%#x, size=%llu", obj.id.v1, obj.size);
        // 与顶层循环同一条规则：长度为 0 或装不下对象头的子对象会让 offset/size 不再推进
        if (obj.size < sizeof(objectHeader)) {
            LOGW("invalid child object size %llu, treat as corrupted", obj.size);
            return NO_INIT;
        }
        if (obj.id == guidFileProperties) {
            LOGD("File Properties Object");
            std::vector<uint8_t> temp(static_cast<size_t>(obj.size));
            if (m_dataSource->readAt(offset + sizeof(objectHeader), temp.data(), temp.size())
                < (ssize_t)temp.size()) {
                return NO_INIT;
                ;
            }

            headerObj.fpObj.id   = obj.id;
            headerObj.fpObj.size = obj.size;
            getGuidByArray(&temp[0], headerObj.fpObj.fileID);
            headerObj.fpObj.fileSize          = U64LE_AT(&temp[16]);
            headerObj.fpObj.creationDate      = U64LE_AT(&temp[24]);
            headerObj.fpObj.dataPacketsCount  = U64LE_AT(&temp[32]);
            headerObj.fpObj.playDuration      = U64LE_AT(&temp[40]);
            headerObj.fpObj.sendDuration      = U64LE_AT(&temp[48]);
            headerObj.fpObj.preroll           = U64LE_AT(&temp[56]);
            headerObj.fpObj.flags             = U32LE_AT(&temp[64]);
            headerObj.fpObj.minDataPacketSize = U32LE_AT(&temp[68]);
            headerObj.fpObj.maxDataPacketSize = U32LE_AT(&temp[72]);
            headerObj.fpObj.maxBitrate        = U32LE_AT(&temp[76]);
            headerObj.fpObj.dump();
        } else if (obj.id == guidStreamProperties) {
            LOGD("Stream Properties Object");
            std::vector<uint8_t> temp(static_cast<size_t>(obj.size));
            if (m_dataSource->readAt(offset + sizeof(objectHeader), temp.data(), temp.size())
                < (ssize_t)temp.size()) {
                return NO_INIT;
            }
            StreamPropertiesObject spObj;
            spObj.id   = obj.id;
            spObj.size = obj.size;
            getGuidByArray(&temp[0], spObj.streamType);
            getGuidByArray(&temp[16], spObj.errorCorrectionType);
            spObj.timeOffset                  = U64LE_AT(&temp[32]);
            spObj.typeSpecificDataLength      = U32LE_AT(&temp[40]);
            spObj.errorCorrectionDataLength   = U32LE_AT(&temp[44]);
            spObj.flags                       = U16LE_AT(&temp[48]);
            spObj.reserved                    = U32LE_AT(&temp[50]);
            spObj.waveFormatEx.formatTag      = U16LE_AT(&temp[54]);
            spObj.waveFormatEx.channels       = U16LE_AT(&temp[56]);
            spObj.waveFormatEx.samplesPerSec  = U32LE_AT(&temp[58]);
            spObj.waveFormatEx.avgBytesPerSec = U32LE_AT(&temp[62]);
            spObj.waveFormatEx.blockAlign     = U16LE_AT(&temp[66]);
            spObj.waveFormatEx.bitsPerSample  = U16LE_AT(&temp[68]);
            spObj.waveFormatEx.cbSize         = U16LE_AT(&temp[70]);
            if (spObj.streamType == guidStreamAudioMedia) {
                LOGI("found Audio Stream Properties Object");
                headerObj.audioStreamObj = spObj;
                foundAudioStream         = true;
                m_audioStreamNumber      = static_cast<uint8_t>(spObj.flags & 0x7F);
                m_bitRate                = (int)spObj.waveFormatEx.avgBytesPerSec * 8;
                m_blockAlign             = (int)spObj.waveFormatEx.blockAlign;
                if (spObj.typeSpecificDataLength > 18 && spObj.waveFormatEx.cbSize > 0) {
                    size_t extraOffset = 54 + 18;
                    size_t maxExtra    = spObj.typeSpecificDataLength - 18;
                    size_t extraSize   = spObj.waveFormatEx.cbSize;
                    if (extraSize > maxExtra) {
                        extraSize = maxExtra;
                    }
                    if (extraOffset + extraSize <= obj.size && extraSize > 0) {
                        m_codecExtraData = std::make_shared<AudioBuffer>(extraSize);
                        memcpy(m_codecExtraData->data(), &temp[extraOffset], extraSize);
                    }
                }
            }
            spObj.dump();
        } else {
            LOGW("Unknown object, skip it");
        }
        offset += obj.size;
        size   -= obj.size;
    }
    if (!foundAudioStream)
        return NO_INIT;

    LOGI("%s: exit", __func__);
    return OK;
}
status_t ASFExtractor::parseDataObject(off64_t startOffset, ASF::DataObject &dataObj)
{
    off64_t offset  = startOffset;
    off64_t dataEnd = startOffset + dataObj.size;
    if (dataObj.size < 26) {
        return NO_INIT;
    }

    uint8_t dataHeader[26];
    if (m_dataSource->readAt(offset, dataHeader, sizeof(dataHeader))
        < (ssize_t)sizeof(dataHeader)) {
        return NO_INIT;
    }
    offset += sizeof(dataHeader);
    getGuidByArray(&dataHeader[0], dataObj.fileID);
    dataObj.totalDataPackets = U64LE_AT(&dataHeader[16]);
    dataObj.reserved         = U16LE_AT(&dataHeader[24]);
    dataObj.fileID.dump();
    LOGI("totalDataPackets=%llu, reserved=%u", dataObj.totalDataPackets, dataObj.reserved);

    std::vector<uint8_t> audioData;
    if (dataObj.size > 0 && dataObj.size < INT32_MAX) {
        audioData.reserve(static_cast<size_t>(dataObj.size));
    }

    size_t packetCount = 0;
    while (offset + 1 <= dataEnd) {
        off64_t packetStart = offset;
        uint8_t firstByte   = 0;
        if (m_dataSource->readAt(offset, &firstByte, 1) < 1) {
            break;
        }
        offset += 1;

        DataPacket pkt;
        uint8_t lengthTypeFlags = 0;
        uint8_t propertyFlags   = 0;
        bool ecPresent          = (firstByte & 0x80) != 0;
        if (ecPresent) {
            pkt.ec.errorCorrectionFlag = firstByte;
            int lenType                = pkt.ec.errorCorrectionFlag & 0x0F;
            int lenBytes               = 0;
            switch (lenType) {
            case PayloadInfoLengthType_0bit:
                lenBytes = 0;
                break;
            case PayloadInfoLengthType_8bit:
                lenBytes = 1;
                break;
            case PayloadInfoLengthType_16bit:
                lenBytes = 2;
                break;
            case PayloadInfoLengthType_32bit:
                lenBytes = 4;
                break;
            default:
                lenBytes = 0;
                break;
            }
            uint32_t ecDataLen = 0;
            if (lenBytes > 0) {
                uint8_t tmp[4] = { 0 };
                if (m_dataSource->readAt(offset, tmp, lenBytes) < lenBytes) {
                    return NO_INIT;
                }
                switch (lenBytes) {
                case 1:
                    ecDataLen = tmp[0];
                    break;
                case 2:
                    ecDataLen = U16LE_AT(&tmp[0]);
                    break;
                case 4:
                    ecDataLen = U32LE_AT(&tmp[0]);
                    break;
                default:
                    ecDataLen = 0;
                    break;
                }
                offset += lenBytes;
            }
            if (ecDataLen > 0) {
                offset += ecDataLen;
            }
            if (m_dataSource->readAt(offset, &lengthTypeFlags, 1) < 1) {
                return NO_INIT;
            }
            offset += 1;
            if (m_dataSource->readAt(offset, &propertyFlags, 1) < 1) {
                return NO_INIT;
            }
            offset += 1;
        } else {
            lengthTypeFlags = firstByte;
            if (m_dataSource->readAt(offset, &propertyFlags, 1) < 1) {
                return NO_INIT;
            }
            offset += 1;
        }

        pkt.info.lengthTypeFlags  = lengthTypeFlags;
        pkt.info.propertyFlags    = propertyFlags;
        int sequenceTypeByte      = pkt.info.getSequenceTypeByte();
        int paddingLengthTypeByte = pkt.info.getPaddingLengthTypeByte();
        int packetLengthTypeByte  = pkt.info.getPacketLengthTypeByte();
        int infoSize = sequenceTypeByte + paddingLengthTypeByte + packetLengthTypeByte + 6;
        if (infoSize < 6) {
            return NO_INIT;
        }
        std::vector<uint8_t> info(static_cast<size_t>(infoSize));
        if (m_dataSource->readAt(offset, info.data(), info.size()) < (ssize_t)info.size()) {
            return NO_INIT;
        }
        offset += info.size();

        auto splitLengthByArray = [](uint32_t &len, int typeByte, uint8_t *array) {
            switch (typeByte) {
            case 1:
                len = array[0];
                break;
            case 2:
                len = U16LE_AT(&array[0]);
                break;
            case 4:
                len = U32LE_AT(&array[0]);
                break;
            default:
                len = 0;
                break;
            }
        };

        splitLengthByArray(pkt.info.packetLength, packetLengthTypeByte, &info[0]);
        splitLengthByArray(pkt.info.sequence, sequenceTypeByte, &info[packetLengthTypeByte]);
        splitLengthByArray(pkt.info.paddingLength, paddingLengthTypeByte,
                           &info[packetLengthTypeByte + sequenceTypeByte]);
        pkt.info.sendTime
            = U32LE_AT(&info[packetLengthTypeByte + sequenceTypeByte + paddingLengthTypeByte]);
        pkt.info.duration
            = U16LE_AT(&info[packetLengthTypeByte + sequenceTypeByte + paddingLengthTypeByte + 4]);

        if (pkt.info.packetLength == 0) {
            pkt.info.packetLength = m_headerObj.fpObj.minDataPacketSize;
        }
        if (m_headerObj.fpObj.maxDataPacketSize > 0
            && pkt.info.packetLength > m_headerObj.fpObj.maxDataPacketSize) {
            LOGW("packet length too large: %u > %u, clamp", pkt.info.packetLength,
                 m_headerObj.fpObj.maxDataPacketSize);
            pkt.info.packetLength = m_headerObj.fpObj.maxDataPacketSize;
        }
        off64_t headerConsumed = offset - packetStart;
        if (pkt.info.packetLength < headerConsumed) {
            LOGW("packet length too small: %u < header %lld, clamp", pkt.info.packetLength,
                 (long long)headerConsumed);
            pkt.info.packetLength = static_cast<uint32_t>(headerConsumed);
        }
        if (pkt.info.packetLength == 0) {
            LOGW("packet length is 0, abort");
            return NO_INIT;
        }
        off64_t packetEnd = packetStart + pkt.info.packetLength;
        if (packetEnd > dataEnd) {
            LOGW("packet end beyond data end, clamp");
            pkt.info.packetLength = static_cast<uint32_t>(dataEnd - packetStart);
            if (pkt.info.packetLength == 0) {
                break;
            }
            packetEnd = dataEnd;
        }
        if (pkt.info.paddingLength > pkt.info.packetLength) {
            LOGW("padding length too large: %u > %u, clamp", pkt.info.paddingLength,
                 pkt.info.packetLength);
            pkt.info.paddingLength = pkt.info.packetLength;
        }

        pkt.info.dump();

        // parsePayloadData 按「一个 ASF 数据包」为单位追加 payload，所以这里按增量切片，
        // 得到的就是解复用边界——wmav2 在 ffmpeg 里没有 parser，整块送解码只能解出开头（#17）。
        const size_t before = audioData.size();
        status_t ret = parsePayloadData(offset, pkt, packetStart, pkt.info.packetLength, audioData);
        if (ret != OK) {
            return NO_INIT;
        }
        if (audioData.size() > before) {
            // 手写回退路径只数音频包，不再常驻逐包 payload：播放期的包边界统一由
            // ASFPacketReader 现场重算（ADR 0003「包为唯一层间单元 / 读过 ≠ 常驻」）。
            ++m_audioPacketCount;
        }

        packetEnd = packetStart + pkt.info.packetLength;
        if (packetEnd <= offset) {
            return NO_INIT;
        }
        offset = packetEnd;
        if (offset > dataEnd) {
            break;
        }

        packetCount++;
        if (dataObj.totalDataPackets > 0 && packetCount >= dataObj.totalDataPackets) {
            break;
        }
    }

    LOGI("%s: exit, packets=%zu, audio=%zu", __func__, packetCount, audioData.size());
    return OK;
}

sdk_utils::status_t ASFExtractor::parseOpaqueData(off64_t stOffset, ASF::DataPacket &dataPacket)
{
    // TODO: parse opaque data
    LOGI("%s: exit", __func__);
    return OK;
}

sdk_utils::status_t ASFExtractor::parsePayloadData(off64_t stOffset, ASF::DataPacket &dataPacket,
                                                   off64_t packetStart, uint32_t packetLen,
                                                   std::vector<uint8_t> &audioData)
{
    off64_t offset    = stOffset;
    off64_t packetEnd = packetStart + packetLen;
    if (packetEnd < offset) {
        return NO_INIT;
    }
    if (dataPacket.info.paddingLength > packetLen) {
        LOGW("padding length too large: %u > %u, clamp", dataPacket.info.paddingLength, packetLen);
        dataPacket.info.paddingLength = packetLen;
    }

    auto readVarUInt = [&](uint32_t &out, int byteCount) -> bool {
        out = 0;
        if (byteCount <= 0) {
            return true;
        }
        if (byteCount > 4) {
            return false;
        }
        if (offset + byteCount > packetEnd) {
            return false;
        }
        uint8_t tmp[4] = { 0 };
        if (m_dataSource->readAt(offset, tmp, byteCount) < byteCount) {
            return false;
        }
        switch (byteCount) {
        case 1:
            out = tmp[0];
            break;
        case 2:
            out = U16LE_AT(&tmp[0]);
            break;
        case 4:
            out = U32LE_AT(&tmp[0]);
            break;
        default:
            out = 0;
            break;
        }
        offset += byteCount;
        return true;
    };

    if (dataPacket.info.isMultiplePayloads()) {
        LOGD("Multiple Payloads Present");
        if (offset + 1 > packetEnd) {
            return NO_INIT;
        }
        uint8_t payloadFlags = 0;
        if (m_dataSource->readAt(offset, &payloadFlags, 1) < 1) {
            return NO_INIT;
        }
        offset += 1;

        int payloadLengthType     = (payloadFlags >> 6) & 0x03;
        int payloadCount          = payloadFlags & 0x3F;
        int payloadLengthTypeByte = 0;
        switch (payloadLengthType) {
        case PayloadInfoLengthType_8bit:
            payloadLengthTypeByte = 1;
            break;
        case PayloadInfoLengthType_16bit:
            payloadLengthTypeByte = 2;
            break;
        case PayloadInfoLengthType_32bit:
            payloadLengthTypeByte = 4;
            break;
        default:
            payloadLengthTypeByte = 0;
            break;
        }

        if (payloadCount <= 0) {
            return NO_INIT;
        }

        for (int i = 0; i < payloadCount; ++i) {
            uint32_t streamNum = 0;
            int streamNumLen   = dataPacket.info.getStreamNumberLengthTypeByte();
            if (streamNumLen == 0) {
                streamNumLen = 1;
            }
            if (!readVarUInt(streamNum, streamNumLen)) {
                return NO_INIT;
            }
            uint8_t streamNumber = static_cast<uint8_t>(streamNum & 0x7F);

            uint32_t mediaObjectNumber = 0;
            int mediaObjNumLenTypeByte = dataPacket.info.getMediaObjectNumberLengthTypeByte();
            if (!readVarUInt(mediaObjectNumber, mediaObjNumLenTypeByte)) {
                return NO_INIT;
            }

            uint32_t offsetIntoMediaObject = 0;
            int offsetIntoMediaObjLenTypeByte
                = dataPacket.info.getOffsetIntoMediaObjectLengthTypeByte();
            if (!readVarUInt(offsetIntoMediaObject, offsetIntoMediaObjLenTypeByte)) {
                return NO_INIT;
            }

            uint32_t replicatedDataLength = 0;
            int replicatedDataLenTypeByte = dataPacket.info.getReplicatedDataLengthTypeByte();
            if (!readVarUInt(replicatedDataLength, replicatedDataLenTypeByte)) {
                return NO_INIT;
            }
            if (replicatedDataLength > 0) {
                if (offset + replicatedDataLength > packetEnd - dataPacket.info.paddingLength) {
                    replicatedDataLength
                        = static_cast<uint32_t>(packetEnd - dataPacket.info.paddingLength - offset);
                }
                offset += replicatedDataLength;
            }

            uint32_t payloadLen = 0;
            if (payloadLengthTypeByte > 0) {
                if (!readVarUInt(payloadLen, payloadLengthTypeByte)) {
                    return NO_INIT;
                }
            } else {
                off64_t remain = packetEnd - dataPacket.info.paddingLength - offset;
                if (remain < 0) {
                    remain = 0;
                }
                if (i != payloadCount - 1) {
                    int remainingPayloads = payloadCount - i;
                    payloadLen            = remainingPayloads > 0
                                   ? static_cast<uint32_t>(remain / remainingPayloads)
                                   : 0;
                } else {
                    payloadLen = static_cast<uint32_t>(remain);
                }
            }

            if (payloadLen > 0) {
                if (offset + payloadLen > packetEnd - dataPacket.info.paddingLength) {
                    payloadLen
                        = static_cast<uint32_t>(packetEnd - dataPacket.info.paddingLength - offset);
                }
                std::vector<uint8_t> payload(payloadLen);
                if (m_dataSource->readAt(offset, payload.data(), payload.size())
                    < (ssize_t)payload.size()) {
                    return NO_INIT;
                }
                if (streamNumber == m_audioStreamNumber) {
                    size_t oldSize = audioData.size();
                    audioData.resize(oldSize + payload.size());
                    memcpy(audioData.data() + oldSize, payload.data(), payload.size());
                }
                offset += payloadLen;
            }
        }
    } else {
        LOGD("Single Payload Present");
        if (offset + 1 > packetEnd) {
            return NO_INIT;
        }
        uint8_t streamNumByte = 0;
        if (m_dataSource->readAt(offset, &streamNumByte, 1) < 1) {
            return NO_INIT;
        }
        offset               += 1;
        uint8_t streamNumber  = streamNumByte & 0x7F;

        uint32_t mediaObjectNumber = 0;
        int mediaObjNumLenTypeByte = dataPacket.info.getMediaObjectNumberLengthTypeByte();
        if (!readVarUInt(mediaObjectNumber, mediaObjNumLenTypeByte)) {
            return NO_INIT;
        }

        uint32_t offsetIntoMediaObject = 0;
        int offsetIntoMediaObjLenTypeByte
            = dataPacket.info.getOffsetIntoMediaObjectLengthTypeByte();
        if (!readVarUInt(offsetIntoMediaObject, offsetIntoMediaObjLenTypeByte)) {
            return NO_INIT;
        }

        uint32_t replicatedDataLength = 0;
        int replicatedDataLenTypeByte = dataPacket.info.getReplicatedDataLengthTypeByte();
        if (!readVarUInt(replicatedDataLength, replicatedDataLenTypeByte)) {
            return NO_INIT;
        }
        if (replicatedDataLength > 0) {
            if (offset + replicatedDataLength > packetEnd - dataPacket.info.paddingLength) {
                replicatedDataLength
                    = static_cast<uint32_t>(packetEnd - dataPacket.info.paddingLength - offset);
            }
            offset += replicatedDataLength;
        }

        off64_t payloadLen64 = packetEnd - dataPacket.info.paddingLength - offset;
        if (payloadLen64 < 0) {
            LOGW("payload length negative, skip packet");
            return OK;
        }
        uint32_t payloadLen = static_cast<uint32_t>(payloadLen64);
        if (payloadLen > 0) {
            std::vector<uint8_t> payload(payloadLen);
            if (m_dataSource->readAt(offset, payload.data(), payload.size())
                < (ssize_t)payload.size()) {
                return NO_INIT;
            }
            if (streamNumber == m_audioStreamNumber) {
                size_t oldSize = audioData.size();
                audioData.resize(oldSize + payload.size());
                memcpy(audioData.data() + oldSize, payload.data(), payload.size());
            }
            offset += payloadLen;
        }
    }

    LOGI("%s: exit", __func__);
    return OK;
}

status_t ASFExtractor::parseIndexObject(off64_t stOffset, ASF::Object &indexObj)
{
    // TODO: parse index object
    LOGI("%s: exit", __func__);
    return OK;
}

void ASF::FilePropertiesObject::dump()
{
    this->id.dump();
    LOGD("objectSize=%llu", this->size);
    this->fileID.dump();
    LOGD("fileSize=%llu", fileSize);
    LOGD("creationDate=%llu", creationDate);
    LOGD("dataPacketsCount=%llu", dataPacketsCount);
    LOGD("playDuration=%llu", playDuration);
    LOGD("sendDuration=%llu", sendDuration);
    LOGD("preroll=%llu", preroll);
    LOGD("flags=%#x", flags);
    LOGD("minDataPacketSize=%u", minDataPacketSize);
    LOGD("maxDataPacketSize=%u", maxDataPacketSize);
    LOGD("maxBitrate=%u", maxBitrate);
}

void ASF::StreamPropertiesObject::dump()
{
    this->id.dump();
    this->streamType.dump();
    this->errorCorrectionType.dump();
    LOGD("timeOffset=%llu", timeOffset);
    LOGD("typeSpecificDataLength=%u", typeSpecificDataLength);
    LOGD("errorCorrectionDataLength=%u", errorCorrectionDataLength);
    LOGD("flags=%#x", flags);
    LOGD("reserved=%u", reserved);
    LOGD("formatTag=%u", waveFormatEx.formatTag);
    LOGD("channels=%u", waveFormatEx.channels);
    LOGD("samplesPerSec=%u", waveFormatEx.samplesPerSec);
    LOGD("avgBytesPerSec=%u", waveFormatEx.avgBytesPerSec);
    LOGD("blockAlign=%u", waveFormatEx.blockAlign);
    LOGD("bitsPerSample=%u", waveFormatEx.bitsPerSample);
    LOGD("cbSize=%u", waveFormatEx.cbSize);
}

void ASF::GUID::dump()
{
    LOGD("GUID: %4x-%2x-%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", v1, v2, v3, v4[0], v4[1], v4[2], v4[3],
         v4[4], v4[5], v4[6], v4[7]);
}

void ASF::PayloadParsingInfomation::dump()
{
    LOGD("lengthTypeFlags=%#x", lengthTypeFlags);
    LOGD("propertyFlags=%#x", propertyFlags);
    LOGD("packetLength=%u", packetLength);
    LOGD("sequence=%u", sequence);
    LOGD("paddingLength=%u", paddingLength);
    LOGD("sendTime=%u", sendTime);
    LOGD("duration=%u", duration);
}
