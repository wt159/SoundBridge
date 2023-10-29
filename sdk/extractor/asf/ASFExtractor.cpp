#include "ASFExtractor.h"
#include "ErrorUtils.h"
#include "LogWrapper.h"
#include <cstring>

#define LOG_TAG "ASFExtractor"

using namespace sdk_utils;
using namespace ASF;

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
{
    m_initCheck = init();
}

ASFExtractor::~ASFExtractor() { }

status_t ASFExtractor::init()
{
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
    status_t ret   = NO_INIT;
    off64_t offset = startOffset;
    ssize_t size   = dataObj.size;
    while (size > 0) {
        uint8_t dataHeader[26];
        if (m_dataSource->readAt(offset, dataHeader, sizeof(dataHeader))
            < (ssize_t)sizeof(dataHeader)) {
            return NO_INIT;
        }
        offset += sizeof(dataHeader);
        size   -= sizeof(dataHeader);
        getGuidByArray(&dataHeader[0], dataObj.fileID);
        dataObj.totalDataPackets = U64LE_AT(&dataHeader[16]);
        dataObj.reserved         = U16LE_AT(&dataHeader[24]);
        dataObj.fileID.dump();
        LOGI("totalDataPackets=%llu, reserved=%u", dataObj.totalDataPackets, dataObj.reserved);
        for (size_t i = 0; i < dataObj.totalDataPackets; i++) {
            uint8_t packetHeader[3];
            if (m_dataSource->readAt(offset, packetHeader, sizeof(packetHeader))
                < (ssize_t)sizeof(packetHeader)) {
                return NO_INIT;
            }
            offset += sizeof(packetHeader);
            size   -= sizeof(packetHeader);
            DataPacket pkt;
            pkt.ec.errorCorrectionFlag = packetHeader[0];
            pkt.ec.firstByteType       = packetHeader[1];
            pkt.ec.secondByteType      = packetHeader[2];
            LOGD("errorCorrectionFlag=%#x, firstByteType=%#x, secondByteType=%#x",
                 pkt.ec.errorCorrectionFlag, pkt.ec.firstByteType, pkt.ec.secondByteType);
            if (pkt.ec.opaquePresent()) {
                ret = parseOpaqueData(offset, pkt);
                if (ret != OK) {
                    return NO_INIT;
                }
            } else {
                ret = parsePayloadData(offset, pkt);
                if (ret != OK) {
                    return NO_INIT;
                }
            }
            dataObj.dataPacketVec.push_back(pkt);
            break;
        }

        break;
    }
    LOGI("%s: exit", __func__);
    return OK;
}

sdk_utils::status_t ASFExtractor::parseOpaqueData(off64_t stOffset, ASF::DataPacket &dataPacket)
{
    // TODO: parse opaque data
    LOGI("%s: exit", __func__);
    return OK;
}

sdk_utils::status_t ASFExtractor::parsePayloadData(off64_t stOffset, ASF::DataPacket &dataPacket)
{
    off64_t offset = stOffset;
    uint8_t flags[2];
    if (m_dataSource->readAt(offset, flags, sizeof(flags)) < (ssize_t)sizeof(flags)) {
        return NO_INIT;
    }
    offset                          += sizeof(flags);
    dataPacket.info.lengthTypeFlags  = flags[0];
    dataPacket.info.propertyFlags    = flags[1];
    int sequenceTypeByte             = dataPacket.info.getSequenceTypeByte();
    int paddingLengthTypeByte        = dataPacket.info.getPaddingLengthTypeByte();
    int packetLengthTypeByte         = dataPacket.info.getPacketLengthTypeByte();
    int infoSize = sequenceTypeByte + paddingLengthTypeByte + packetLengthTypeByte + 6;
    uint8_t info[infoSize];
    if (m_dataSource->readAt(offset, info, sizeof(info)) < (ssize_t)sizeof(info)) {
        return NO_INIT;
    }
    offset                  += sizeof(info);
    auto splitLengthByArray  = [](uint32_t &len, int typeByte, uint8_t *array) {
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
    splitLengthByArray(dataPacket.info.packetLength, packetLengthTypeByte, &info[0]);
    splitLengthByArray(dataPacket.info.sequence, sequenceTypeByte, &info[packetLengthTypeByte]);
    splitLengthByArray(dataPacket.info.paddingLength, paddingLengthTypeByte,
                       &info[packetLengthTypeByte + sequenceTypeByte]);
    dataPacket.info.sendTime
        = U32LE_AT(&info[packetLengthTypeByte + sequenceTypeByte + paddingLengthTypeByte]);
    dataPacket.info.duration
        = U16LE_AT(&info[packetLengthTypeByte + sequenceTypeByte + paddingLengthTypeByte + 4]);
    dataPacket.info.dump();
    if (dataPacket.info.isMultiplePayloads()) {
        LOGD("Multiple Payloads Present");
        // TODO: parse multiple payloads
    } else {
        LOGD("Single Payload Present");
        int size = 1;
        int mediaObjNumLenTypeByte
            = dataPacket.info.getMediaObjectNumberLengthTypeByte();
        int offsetIntoMediaObjLenTypeByte
            = dataPacket.info.getOffsetIntoMediaObjectLengthTypeByte();
        int replicatedDataLenTypeByte = dataPacket.info.getReplicatedDataLengthTypeByte();
        size += mediaObjNumLenTypeByte + offsetIntoMediaObjLenTypeByte + replicatedDataLenTypeByte;
        uint8_t payload[size];
        if (m_dataSource->readAt(offset, payload, sizeof(payload)) < (ssize_t)sizeof(payload)) {
            return NO_INIT;
        }
        offset += sizeof(payload);
        splitLengthByArray(dataPacket.payloadData.single.mediaObjectNumber,
                           mediaObjNumLenTypeByte, &payload[0]);
        splitLengthByArray(dataPacket.payloadData.single.offsetIntoMediaObject, offsetIntoMediaObjLenTypeByte,
                           &payload[mediaObjNumLenTypeByte]);
        splitLengthByArray(dataPacket.payloadData.single.replicatedDataLength, replicatedDataLenTypeByte,
                           &payload[mediaObjNumLenTypeByte + offsetIntoMediaObjLenTypeByte]);
        LOGD("mediaObjectNumber=%u, offsetIntoMediaObject=%u, replicatedDataLength=%u",
             dataPacket.payloadData.single.mediaObjectNumber,
             dataPacket.payloadData.single.offsetIntoMediaObject,
             dataPacket.payloadData.single.replicatedDataLength);
        // TODO: parse single payload
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
