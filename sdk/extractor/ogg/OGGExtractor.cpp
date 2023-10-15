#include "ByteUtils.h"
#include "LogWrapper.h"
#include "OGGExtractor.h"
#include "type_name.hpp"
#include <cstring>

#define LOG_TAG "OGGExtractor"

using namespace sdk_utils;
using namespace OGG;

constexpr uint16_t kMaxCodecParameterNum = 15;

OGGExtractor::OGGExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_metaBuf(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
{
    m_initCheck = init();
}

OGGExtractor::~OGGExtractor() { }

status_t OGGExtractor::init()
{
    int offset = 0;
    while (1) {
        OGG::Page page;
        int headerSize = 27;
        char header[headerSize];
        if (m_dataSource->readAt(offset, header, headerSize) < headerSize) {
            break;
        }

        memcpy(page.header.id, header, 4);
        if (memcmp(page.header.id, PAGE_IDENTIFICATION, 4) != 0) {
            LOGE("this is invalid format");
            m_validFormat = false;
            break;
        }
        m_validFormat          = true;
        page.header.version    = header[4];
        page.header.headerType = header[5];
        if (page.header.headerType & OGG::HeaderType::firstPage) {
            LOGD("first page");
        }
        if (page.header.headerType & OGG::HeaderType::lastPage) {
            LOGD("last page");
        }
        if (page.header.headerType & OGG::HeaderType::newPacket) {
            LOGD("new packet");
        }
        page.header.granulePosition     = U64LE_AT((uint8_t *)header + 6);
        page.header.serialNumber        = U32LE_AT((uint8_t *)header + 14);
        page.header.pageSequenceNumber  = U32LE_AT((uint8_t *)header + 18);
        page.header.checkSum            = U32LE_AT((uint8_t *)header + 22);
        page.header.numPageSegments     = header[26];
        offset                         += headerSize;
        uint8_t numSegments             = page.header.numPageSegments;
        for (int i = 0; i < numSegments; i++) {
            if (m_dataSource->readAt(offset, &page.header.segmentTable[i], 1) < 1) {
                m_validFormat = false;
                break;
            }
            offset += 1;
        }
        for (int i = 0; i < numSegments; i++) {
            PagePacket packet;
            uint8_t segmentSize = page.header.segmentTable[i];
            packet.data         = std::make_shared<AudioBuffer>(segmentSize);
            if (m_dataSource->readAt(offset, packet.data->data(), segmentSize) < segmentSize) {
                m_validFormat = false;
                break;
            }
            offset += segmentSize;
            page.packets.push_back(packet);
        }
        m_pageVec.push_back(page);
    }

    if (!m_validFormat) {
        return NO_INIT;
    }

    PagePacket *codecPkt = &(m_pageVec[0].packets[0]);
    if (codecPkt->data->size() < 5) {
        LOGE("codecPkt size error");
        return NO_INIT;
    }

    if (!memcmp(codecPkt->data->data(), "\177fLaC", 5)) {
        m_audioCodecID = AUDIO_CODEC_ID_FLAC;
    } else if (!memcmp(codecPkt->data->data(), "OpusHead", 8)) {
        m_audioCodecID = AUDIO_CODEC_ID_OPUS;
    } else if (!memcmp(codecPkt->data->data(), "\x01vorbis", 7)) {
        m_audioCodecID = AUDIO_CODEC_ID_VORBIS;
    } else if (!memcmp(codecPkt->data->data(), "Speex   ", 8)) {
        m_audioCodecID = AUDIO_CODEC_ID_SPEEX;
    } else if (!memcmp(codecPkt->data->data(), "CELT    ", 8)) {
        m_audioCodecID = AUDIO_CODEC_ID_CELT;
    } else if (!memcmp(codecPkt->data->data(), "PCM     ", 8)) {
        m_audioCodecID = AUDIO_CODEC_ID_NONE;
    } else {
        LOGE("unknown codec id");
        return NO_INIT;
    }

    uint32_t codecSerialNum = m_pageVec[0].header.serialNumber;
    LOGD("codecSerialNum = %u", codecSerialNum);
    uint64_t dataSize = 0;
    std::vector<AudioBuffer::AudioBufferPtr> codecPktVec;
    codecPktVec.clear();

    for (auto &pg : m_pageVec) {
        if (pg.header.serialNumber != codecSerialNum) {
            continue;
        }
        for (auto &pkt : pg.packets) {
            dataSize += pkt.data->size();
            codecPktVec.push_back(pkt.data);
        }
    }
    LOGD("dataSize = %llu", dataSize);

    m_metaBuf                = std::make_shared<AudioBuffer>(dataSize);
    uint64_t offsetInMetaBuf = 0;
    for (auto &pkt : codecPktVec) {
        memcpy(m_metaBuf->data() + offsetInMetaBuf, pkt->data(), pkt->size());
        offsetInMetaBuf += pkt->size();
    }

    return OK;
}
