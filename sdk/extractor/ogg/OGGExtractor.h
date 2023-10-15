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
    virtual status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual ~OGGExtractor();

private:
    status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    std::vector<OGG::Page> m_pageVec;
};