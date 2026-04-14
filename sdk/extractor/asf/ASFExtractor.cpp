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
    , m_audioStreamNumber(0)
    , m_bitRate(0)
    , m_blockAlign(0)
    , m_codecExtraData(nullptr)
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
    while (offset <= fileSize) {
        uint8_t objectHeader[24];
        if (m_dataSource->readAt(offset, objectHeader, sizeof(objectHeader))
            < (ssize_t)sizeof(objectHeader)) {
            m_validFormat = false;
            break;
        }
        offset += sizeof(objectHeader);
        Object obj;
        getGuidObjByArray(objectHeader, obj);
        LOGI("id=%#x, size=%llu", obj.id.v1, obj.size);
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
        offset += (obj.size - sizeof(objectHeader));
    }

    if (!m_validFormat) {
        return NO_INIT;
    }

    return OK;
}

int ASFExtractor::avioRead(void *opaque, uint8_t *buf, int buf_size)
{
    if (!opaque || !buf || buf_size <= 0) {
        return AVERROR_EOF;
    }
    AvioData *ctx = reinterpret_cast<AvioData *>(opaque);
    if (!ctx->source) {
        return AVERROR_EOF;
    }
    ssize_t n = ctx->source->readAt(ctx->pos, buf, static_cast<size_t>(buf_size));
    if (n <= 0) {
        return AVERROR_EOF;
    }
    ctx->pos += n;
    return static_cast<int>(n);
}

int64_t ASFExtractor::avioSeek(void *opaque, int64_t offset, int whence)
{
    if (!opaque) {
        return -1;
    }
    AvioData *ctx = reinterpret_cast<AvioData *>(opaque);
    if (!ctx->source) {
        return -1;
    }
    if (whence == AVSEEK_SIZE) {
        return ctx->size;
    }

    int64_t newPos = ctx->pos;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        newPos = offset;
        break;
    case SEEK_CUR:
        newPos = ctx->pos + offset;
        break;
    case SEEK_END:
        newPos = ctx->size + offset;
        break;
    default:
        return -1;
    }

    if (newPos < 0) {
        newPos = 0;
    }
    if (ctx->size >= 0 && newPos > ctx->size) {
        newPos = ctx->size;
    }
    ctx->pos = newPos;
    return ctx->pos;
}

status_t ASFExtractor::initWithFFmpegDemux()
{
    off64_t fileSize = 0;
    m_dataSource->getSize(&fileSize);
    if (fileSize <= 0) {
        LOGW("initWithFFmpegDemux invalid file size");
        return NO_INIT;
    }

    AvioData ioCtxData;
    ioCtxData.source = m_dataSource;
    ioCtxData.pos    = 0;
    ioCtxData.size   = static_cast<int64_t>(fileSize);

    const int ioBufferSize = 64 * 1024;
    uint8_t *ioBuffer      = static_cast<uint8_t *>(av_malloc(ioBufferSize));
    if (!ioBuffer) {
        LOGE("initWithFFmpegDemux av_malloc failed");
        return NO_MEMORY;
    }

    AVIOContext *avioCtx
        = avio_alloc_context(ioBuffer, ioBufferSize, 0, &ioCtxData, &ASFExtractor::avioRead,
                             nullptr, &ASFExtractor::avioSeek);
    if (!avioCtx) {
        av_free(ioBuffer);
        LOGE("initWithFFmpegDemux avio_alloc_context failed");
        return NO_MEMORY;
    }

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        avio_context_free(&avioCtx);
        LOGE("initWithFFmpegDemux avformat_alloc_context failed");
        return NO_MEMORY;
    }
    fmt->pb     = avioCtx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        LOGW("initWithFFmpegDemux avformat_open_input failed: %d", ret);
        avformat_free_context(fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        LOGW("initWithFFmpegDemux avformat_find_stream_info failed: %d", ret);
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    int audioIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIndex < 0) {
        LOGW("initWithFFmpegDemux no audio stream");
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    AVStream *audioStream = fmt->streams[audioIndex];
    if (!audioStream || !audioStream->codecpar) {
        LOGW("initWithFFmpegDemux invalid audio stream");
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_INIT;
    }

    AVCodecParameters *par = audioStream->codecpar;
    m_audioCodecID         = static_cast<AudioCodecID>(par->codec_id);
    m_audioSpec.sampleRate = par->sample_rate;
    m_audioSpec.numChannel = par->channels;
    m_audioSpec.bitsPerSample
        = par->bits_per_coded_sample > 0 ? par->bits_per_coded_sample : par->bits_per_raw_sample;
    m_audioSpec.bytesPerSample
        = m_audioSpec.bitsPerSample > 0 ? (m_audioSpec.bitsPerSample + 7) / 8 : 0;
    m_audioSpec.format = getAudioFormatByBitPreSample(m_audioSpec.bitsPerSample);
    if (fmt->duration > 0) {
        m_audioSpec.durationMs = static_cast<uint64_t>(fmt->duration / 1000);
    }
    m_bitRate    = static_cast<int>(par->bit_rate);
    m_blockAlign = static_cast<int>(par->block_align);
    if (par->extradata && par->extradata_size > 0) {
        m_codecExtraData = std::make_shared<AudioBuffer>(par->extradata_size);
        memcpy(m_codecExtraData->data(), par->extradata, par->extradata_size);
    }

    std::vector<uint8_t> audioData;
    std::vector<AudioBuffer::AudioBufferPtr> packetizedAudioData;
    if (fileSize > 0 && fileSize < INT32_MAX) {
        audioData.reserve(static_cast<size_t>(fileSize));
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&fmt);
        avio_context_free(&avioCtx);
        return NO_MEMORY;
    }

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audioIndex && pkt->size > 0) {
            size_t oldSize = audioData.size();
            audioData.resize(oldSize + static_cast<size_t>(pkt->size));
            memcpy(audioData.data() + oldSize, pkt->data, pkt->size);

            AudioBuffer::AudioBufferPtr packet = std::make_shared<AudioBuffer>(pkt->size);
            memcpy(packet->data(), pkt->data, pkt->size);
            packetizedAudioData.push_back(packet);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    avformat_close_input(&fmt);
    avio_context_free(&avioCtx);

    if (audioData.empty() || packetizedAudioData.empty()) {
        LOGW("initWithFFmpegDemux no audio payload collected");
        return NO_INIT;
    }

    m_metaBuf = std::make_shared<AudioBuffer>(audioData.size());
    memcpy(m_metaBuf->data(), audioData.data(), audioData.size());
    m_packetizedMetaBuf = packetizedAudioData;
    m_validFormat       = true;
    LOGI("initWithFFmpegDemux ok, codec=%#x, payload=%zu", m_audioCodecID, audioData.size());
    normalizeAudioSpec(m_audioSpec);
    return OK;
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
        if (obj.id == guidFileProperties) {
            LOGD("File Properties Object");
            uint8_t temp[obj.size];
            if (m_dataSource->readAt(offset + sizeof(objectHeader), temp, sizeof(temp))
                < (ssize_t)sizeof(temp)) {
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
            uint8_t temp[obj.size];
            if (m_dataSource->readAt(offset + sizeof(objectHeader), temp, sizeof(temp))
                < (ssize_t)sizeof(temp)) {
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

        status_t ret = parsePayloadData(offset, pkt, packetStart, pkt.info.packetLength, audioData);
        if (ret != OK) {
            return NO_INIT;
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

    if (!audioData.empty()) {
        m_metaBuf = std::make_shared<AudioBuffer>(audioData.size());
        memcpy(m_metaBuf->data(), audioData.data(), audioData.size());
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
