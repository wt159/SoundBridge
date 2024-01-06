#include "ByteUtils.h"
#include "LogWrapper.h"
#include "M4AExtractor.h"

#define LOG_TAG "M4AExtractor"

using namespace sdk_utils;

namespace M4A {
std::unordered_map<std::string, MetaDataBoxType> MetaDataBlockTypeMap = {
    { std::string("ftyp"), MetaDataBoxType::ftyp }, { std::string("mdat"), MetaDataBoxType::mdat },
    { std::string("moov"), MetaDataBoxType::moov }, { std::string("pnot"), MetaDataBoxType::pnot },
    { std::string("udta"), MetaDataBoxType::udta }, { std::string("uuid"), MetaDataBoxType::uuid },
    { std::string("moof"), MetaDataBoxType::moof }, { std::string("free"), MetaDataBoxType::free },
    { std::string("skip"), MetaDataBoxType::skip }, { std::string("jp2"), MetaDataBoxType::jp2 },
    { std::string("wide"), MetaDataBoxType::wide }, { std::string("load"), MetaDataBoxType::load },
    { std::string("ctab"), MetaDataBoxType::ctab }, { std::string("imap"), MetaDataBoxType::imap },
    { std::string("matt"), MetaDataBoxType::matt }, { std::string("kmat"), MetaDataBoxType::kmat },
    { std::string("clip"), MetaDataBoxType::clip }, { std::string("crgn"), MetaDataBoxType::crgn },
    { std::string("sync"), MetaDataBoxType::sync }, { std::string("chap"), MetaDataBoxType::chap },
    { std::string("tmcd"), MetaDataBoxType::tmcd }, { std::string("scpt"), MetaDataBoxType::scpt },
    { std::string("ssrc"), MetaDataBoxType::ssrc }, { std::string("pict"), MetaDataBoxType::pict },
};
}

M4AExtractor::M4AExtractor(DataSourceBase *source)
    : m_dataSource(source)
    , m_audioCodecID(AUDIO_CODEC_ID_NONE)
    , m_metaBuf(nullptr)
    , m_initCheck(NO_INIT)
    , m_validFormat(false)
    , m_spec()
    , m_headerVec()
{
    m_headerVec.clear();
    m_initCheck = init();
}

M4AExtractor::~M4AExtractor() { }

status_t M4AExtractor::init()
{
    int offset = 0;
    while (1) {
        M4A::MetaDataBox block;
        int headerSize = 8;
        char header[headerSize];
        if (m_dataSource->readAt(offset, header, headerSize) < headerSize) {
            break;
        }

        std::string typeStr(header + 4, 4);
        block.header.size = U32_AT((uint8_t *)header);
        if (M4A::MetaDataBlockTypeMap.find(typeStr) == M4A::MetaDataBlockTypeMap.end()) {
            break;
        }
        m_validFormat     = true;
        block.header.type = M4A::MetaDataBlockTypeMap.at(std::string(header + 4, 4));
        LOGI("size = %d, type: %s", block.header.size, typeStr.c_str());
        block.header.offset  = offset;
        offset              += block.header.size;
        m_headerVec.push_back(block);
    }

    if (!m_validFormat) {
        LOGE("m_validFormat = %d", m_validFormat);
        return NO_INIT;
    }

    m_audioCodecID = AUDIO_CODEC_ID_AAC;

#if 0
    for (auto &block : m_headerVec) {
        if (block.header.type == M4A::MetaDataBoxType::mdat) {
            ssize_t bufSize = block.header.size - 8;
            m_metaBuf       = std::make_shared<AudioBuffer>(bufSize);
            if (m_dataSource->readAt(block.header.offset + 8, m_metaBuf->data(), bufSize)
                < bufSize) {
                LOGE("readAt failed");
                return NO_MEMORY;
            }
            break;
        }
    }
#else
    off64_t bufSize = 0;
    m_dataSource->getSize(&bufSize);
    m_metaBuf       = std::make_shared<AudioBuffer>(bufSize);
    if (m_dataSource->readAt(0, m_metaBuf->data(), bufSize) < bufSize) {
        LOGE("readAt failed");
        return NO_MEMORY;
    }
#endif
    return OK;
}
