#pragma once

#include "ASFPacketReader.h"
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

// asf / wma 提取器（票面 #64 / ADR 0003，同一提取器承接两个扩展名）。
//
// init 期只做一次只读扫描：逐包累加标称时长（包时间轴优先，容器声明只兜底），扫描结束
// 就把 demux 关掉，只留 O(1) 定长摘要。asf 的尾部 Simple Index Object 由 ffmpeg demux
// 内部按需读取，自仓不缓存整首索引、不建 seek 表。手写 object parser 只在 ffmpeg demux
// 失败时回退，用于解析 spec 与快速终止判定，不再产出常驻 payload。
//
// 播放期的包边界由 ASFPacketReader 现场重算：wmav2 在 ffmpeg 里没有 parser，边界只能由
// 提取器逐包给出。.asf / .wma 共用同一个 reader，是容器差异，不是契约分叉。
class ASFExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit ASFExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual int getBitRate() { return m_bitRate; }
    virtual int getBlockAlign() { return m_blockAlign; }
    virtual ~ASFExtractor();

    // 包工厂的单包面（#64 / ADR 0003）：总数事先未知，边界按 offset 现场重算，
    // 任何「先把整份文件数一遍再建表」的做法都会把常驻内存按包数放大。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual bool seekPacketByte(int64_t offset);
    virtual int64_t packetBytePosition() const;

    // 分配记账：init 不持有 payload，播放期的 demux 面只持 AVIO 缓冲（判据公式里的
    // 独立项，口径上不混进这个数）；交出去的包归解码侧 alloc/free 记账。
    virtual size_t heldPayloadBytes() const { return 0; }

    // 现场重算包边界的 CPU 成本（独立指标，不设阈值；见规格第 5 节末段）。
    virtual uint64_t packetBoundaryMicros() const
    {
        return m_reader != nullptr ? m_reader->boundaryMicros() : 0;
    }

private:
    sdk_utils::status_t init();
    sdk_utils::status_t initWithFFmpegDemux();
    // demux 面在播放期创建（规格 4.3 所有权清单）。
    ASFPacketReader *reader();
    // 以下手写 object parser 只在 ffmpeg demux 失败时回退使用：解析 spec 并做快速终止判定，
    // 不再产出常驻 payload；播放期的包边界统一由 ASFPacketReader 现场重算。
    void getGuidObjByArray(const uint8_t *array, ASF::Object &obj);
    void getGuidByArray(const uint8_t *array, ASF::GUID &guid);
    sdk_utils::status_t parseHeaderObject(off64_t offset, ASF::HeaderObject &headerObj);
    sdk_utils::status_t parseDataObject(off64_t offset, ASF::DataObject &dataObj);
    sdk_utils::status_t parseOpaqueData(off64_t offset, ASF::DataPacket &dataPacket);
    sdk_utils::status_t parsePayloadData(off64_t offset, ASF::DataPacket &dataPacket,
                                         off64_t packetStart, uint32_t packetLen,
                                         std::vector<uint8_t> &audioData);
    sdk_utils::status_t parseIndexObject(off64_t offset, ASF::Object &indexObj);

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    // 手写回退路径的 O(1) 解析上下文（Header / Data 对象头、音频流号），无随包数增长的常驻表。
    ASF::HeaderObject m_headerObj;
    ASF::DataObject m_dataObj;
    uint8_t m_audioStreamNumber;
    int m_bitRate;
    int m_blockAlign;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    int64_t m_fileSize;
    // init 只读扫描数到的音频包数：只用来判「有没有音频」，不随播放增长。
    size_t m_audioPacketCount;
    std::unique_ptr<ASFPacketReader> m_reader;
    size_t m_nextPacketIndex;
    // 本段包序列的起点（顺序重放 seek 之后为 0）。
    int64_t m_sequenceOffset;
};
