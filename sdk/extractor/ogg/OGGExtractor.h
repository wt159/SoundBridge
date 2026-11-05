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
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual DataSourceBase *getDataSource() { return m_dataSource; }
    virtual off64_t getDataSize() { return m_dataSize; }
    virtual off64_t getAudioDataOffset() { return 0; }
    // 分配记账：页表 + 每页的包（payload 就存在这里）+ 包指针槽位。
    // 当前链路把整份文件的页都留在 m_pageVec 里，所以这个数约等于文件大小。
    virtual size_t heldPayloadBytes() const
    {
        size_t total = bufferBytes(m_metaBuf) + vectorFootprint(m_pageVec);
        for (size_t i = 0; i < m_pageVec.size(); ++i) {
            total += vectorFootprint(m_pageVec[i].packets);
            for (size_t j = 0; j < m_pageVec[i].packets.size(); ++j) {
                total += bufferBytes(m_pageVec[i].packets[j].data);
            }
        }
        return total + ExtractorHelper::heldPayloadBytes();
    }
    virtual ~OGGExtractor();

private:
    sdk_utils::status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    std::vector<OGG::Page> m_pageVec;
    off64_t m_dataSize;
};
