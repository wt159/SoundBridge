#pragma once

#include "AACFrameReader.h"
#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>
#include <vector>

struct ADTSHeader {
    uint16_t syncWord              : 12;
    uint8_t id                     : 1;
    uint8_t layer                  : 2;
    uint8_t protectionAbsent       : 1;
    uint8_t profile                : 2;
    uint8_t samplingFrequencyIndex : 4;
    uint8_t privateBit             : 1;
    uint8_t channelConfiguration   : 3;
    uint8_t originalCopy           : 1;
    uint8_t home                   : 1;
    uint8_t copyrightIDBit         : 1;
    uint8_t copyrightIDStart       : 1;
    uint16_t frameLength           : 13;
    uint16_t bufferFullness        : 11;
    uint8_t numRawDataBlocks       : 2;
};

struct ADIFHeader {
    uint32_t id                : 32;
    uint8_t copyrightIDPresent : 1;
    uint8_t copyrightID[9];
    uint8_t originalCopy             : 1;
    uint8_t home                     : 1;
    uint8_t bitStreamType            : 1;
    uint32_t bitRate                 : 23;
    uint8_t numProgramConfigElements : 4;
    uint32_t bufferFullness          : 20;
};

union AACHeader {
    ADTSHeader adts;
    ADIFHeader adif;
};

class AACExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit AACExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData();
    DataSourceBase *getDataSource() override { return m_dataSource; }
    // getDataSize()/getAudioDataOffset() 覆写 ExtractorHelper 的既有 off64_t 虚函数
    // （master 即有，不能改签名）；成员改用 int64_t 后，在既有边界处显式转回 off64_t。
    virtual off64_t getDataSize() { return static_cast<off64_t>(m_dataSize); }
    virtual off64_t getAudioDataOffset() { return static_cast<off64_t>(m_audioStart); }

    // —— 包工厂的单包面（#62 / ADR 0003）——
    // 每个 ADTS 帧是一包，总数事先未知（kUnknownPacketCount = 读到为止）。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);
    virtual bool seekPacketByte(int64_t offset);
    virtual int64_t packetBytePosition() const;

    // 分配记账：扫描缓冲是 O(1)。旧整块视图被仪器请求过时才多一份。
    virtual size_t heldPayloadBytes() const
    {
        return bufferBytes(m_metaBuf)
            + (m_reader != nullptr ? m_reader->heldBytes() : 0);
    }
    virtual ~AACExtractor();

private:
    sdk_utils::status_t init();
    AACFrameReader *reader();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    AACHeader m_header;
    // ADTS 帧区间：ID3v2 前缀之后到文件尾。音频负载不含标签（规格 7.3 M6）。
    int64_t m_dataSize;
    int64_t m_audioStart;
    std::unique_ptr<AACFrameReader> m_reader;
    size_t m_nextPacketIndex;
    int64_t m_sequenceOffset;
};
