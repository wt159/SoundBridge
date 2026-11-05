#pragma once

#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

namespace OGG {

#define PAGE_IDENTIFICATION  "OggS"
#define PAGE_SEGMENT_NUM_MAX 255

enum HeaderType : uint8_t {
    newPacket = 0x01,
    firstPage = 0x02,
    lastPage  = 0x04,
};
struct PageHeader {
    uint8_t id[4];
    uint8_t version;
    uint8_t headerType;
    uint64_t granulePosition;
    uint32_t serialNumber;
    uint32_t pageSequenceNumber;
    uint32_t checkSum;
    uint8_t numPageSegments;
    uint8_t segmentTable[PAGE_SEGMENT_NUM_MAX];
    bool isNewPacket() { return headerType & HeaderType::newPacket; }
    bool isFirstPage() { return headerType & HeaderType::firstPage; }
    bool isLastPage() { return headerType & HeaderType::lastPage; }
};

struct PagePacket {
    AudioBuffer::AudioBufferPtr data;
};

struct Page {
    PageHeader header;
    std::vector<PagePacket> packets;
};

}

class OGGExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit OGGExtractor(DataSourceBase *source);
    static bool sniff(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    // ogg 的负载由 libvorbisfile / libFLAC 直接走 DataSource 读，不经过整块视图。
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return nullptr; }
    virtual DataSourceBase *getDataSource() { return m_dataSource; }
    virtual off64_t getDataSize() { return m_dataSize; }
    virtual off64_t getAudioDataOffset() { return 0; }
    // 分配记账：init 扫描只留 O(1) 摘要，不再保留页表与每页的包（ADR 0003）。
    virtual size_t heldPayloadBytes() const { return ExtractorHelper::heldPayloadBytes(); }
    virtual ~OGGExtractor();

private:
    sdk_utils::status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    off64_t m_dataSize;
};
