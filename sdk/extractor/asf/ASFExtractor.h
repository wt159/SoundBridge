#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ASF {

struct GUID {
    uint32_t v1;
    uint16_t v2;
    uint16_t v3;
    uint8_t v4[8];
    bool operator==(const GUID &other) const
    {
        return v1 == other.v1 && v2 == other.v2 && v3 == other.v3
            && memcmp(v4, other.v4, sizeof(v4)) == 0;
    }
    void dump();
};

struct Object {
    GUID id;
    uint64_t size;
    virtual ~Object() { }
};

struct FilePropertiesObject : public Object {
    GUID fileID;
    uint64_t fileSize;
    uint64_t creationDate;
    uint64_t dataPacketsCount;
    uint64_t playDuration;
    uint64_t sendDuration;
    uint64_t preroll;
    uint32_t flags;
    uint32_t minDataPacketSize;
    uint32_t maxDataPacketSize;
    uint32_t maxBitrate;

    void dump();
};

struct WAVEFormatEx {
    uint16_t formatTag;
    uint16_t channels;
    uint32_t samplesPerSec;
    uint32_t avgBytesPerSec;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    uint16_t cbSize;
};
struct StreamPropertiesObject : public Object {
    GUID streamType;
    GUID errorCorrectionType;
    uint64_t timeOffset;
    uint32_t typeSpecificDataLength;
    uint32_t errorCorrectionDataLength;
    uint16_t flags;
    uint32_t reserved;
    WAVEFormatEx waveFormatEx;

    void dump();
};

struct HeaderObject : public Object {
    uint32_t objectCount;
    uint8_t reserved1;
    uint8_t reserved2;
    FilePropertiesObject fpObj;
    StreamPropertiesObject audioStreamObj;
};

struct ErrorCorrectionData {
    /*
     * bits:
     * 0~3   (error Correction Data Length Type)
     * 4     (Opaque Present)
     * 5~6   (Error Correction Length Type)
     * 7     (Error Correction Present)
     */
    uint8_t errorCorrectionFlag;
    /* bits: 0~3 Type 4~7 Number */
    uint8_t firstByteType;
    uint8_t secondByteType;

    bool opaquePresent() const { return errorCorrectionFlag & 0x10; }
    bool errorCorrectionPresent() const { return errorCorrectionFlag & 0x80; }
};
enum {
    PayloadInfoLengthType_0bit  = 0, /* not exist */
    PayloadInfoLengthType_8bit  = 1,
    PayloadInfoLengthType_16bit = 2,
    PayloadInfoLengthType_32bit = 3,
};
struct PayloadParsingInfomation {
    /* bit:
     * 0  (Multiple Payloads Present)
     * 1~2(sequence Type)
     * 3~4(padding Length Type)
     * 5~6(packet Length Type)
     * 7  (Error Correction Present)
     */
    uint8_t lengthTypeFlags;
    /* bit:
     * 0~1(replicated Data Length Type)
     * 2~3(offset Into Media Object Length Type)
     * 4~5(media Object Number Length Type)
     * 6~7(stream Number Length Type)
     */
    uint8_t propertyFlags;
    uint32_t packetLength;
    uint32_t sequence;
    uint32_t paddingLength;
    uint32_t sendTime;
    uint16_t duration;

    bool isMultiplePayloads() const { return lengthTypeFlags & 0x01; }
    int getByteByType(int val) const
    {
        switch (val) {
        case PayloadInfoLengthType_0bit:
            return 0;
        case PayloadInfoLengthType_8bit:
            return 1;
        case PayloadInfoLengthType_16bit:
            return 2;
        case PayloadInfoLengthType_32bit:
            return 4;
        default:
            break;
        }
        return 0;
    }
    int getSequenceTypeByte() const { return getByteByType((lengthTypeFlags >> 1) & 0x03); }
    int getPaddingLengthTypeByte() const { return getByteByType((lengthTypeFlags >> 3) & 0x03); }
    int getPacketLengthTypeByte() const { return getByteByType((lengthTypeFlags >> 5) & 0x03); }
    int getReplicatedDataLengthTypeByte() const
    {
        return getByteByType((propertyFlags >> 0) & 0x03);
    }
    int getOffsetIntoMediaObjectLengthTypeByte() const
    {
        return getByteByType((propertyFlags >> 2) & 0x03);
    }
    int getMediaObjectNumberLengthTypeByte() const
    {
        return getByteByType((propertyFlags >> 4) & 0x03);
    }
    int getStreamNumberLengthTypeByte() const { return getByteByType((propertyFlags >> 6) & 0x03); }
    void dump();
};
struct SinglePayload {
    /* bits: 0~6(stream number) 7(key frame bit) */
    uint8_t streamNumber;
    uint32_t mediaObjectNumber;
    uint32_t offsetIntoMediaObject;
    uint32_t replicatedDataLength;
    uint8_t *replicatedData;
    uint8_t *payloadData;
};
struct MultiplePayload {
    uint8_t payloadFlags;
    struct payload {
        uint8_t streamNumber;
        uint32_t mediaObjectNumber;
        uint32_t offsetIntoMediaObject;
        uint32_t replicatedDataLength;
        uint8_t *replicatedData;
        uint32_t payloadLength;
        uint8_t *payloadData;
    };
    std::vector<payload> payloadVec;
};
struct PayloadData {
    SinglePayload single;
    MultiplePayload multiple;
};
struct PaddingData {
    uint8_t *paddingData;
};
struct OpaqueData {
    uint8_t *opaqueData;
};

struct DataPacket {
    ErrorCorrectionData ec;
    PayloadParsingInfomation info;
    PayloadData payloadData;
    OpaqueData opaqueData;
    PaddingData paddingData;
};

struct DataObject : public Object {
    GUID fileID;
    uint64_t totalDataPackets;
    uint32_t reserved;
    std::vector<DataPacket> dataPacketVec;
};

}

class ASFExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit ASFExtractor(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual ~ASFExtractor();

private:
    sdk_utils::status_t init();
    void getGuidObjByArray(const uint8_t *array, ASF::Object &obj);
    void getGuidByArray(const uint8_t *array, ASF::GUID &guid);
    sdk_utils::status_t parseHeaderObject(off64_t offset, ASF::HeaderObject &headerObj);
    sdk_utils::status_t parseDataObject(off64_t offset, ASF::DataObject &dataObj);
    sdk_utils::status_t parseOpaqueData(off64_t offset, ASF::DataPacket &dataPacket);
    sdk_utils::status_t parsePayloadData(off64_t offset, ASF::DataPacket &dataPacket);
    sdk_utils::status_t parseIndexObject(off64_t offset, ASF::Object &indexObj);

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    ASF::HeaderObject m_headerObj;
    ASF::DataObject m_dataObj;
};