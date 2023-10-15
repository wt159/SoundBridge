#pragma once
#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <unordered_map>
#include <vector>

namespace M4A {
enum MetaDataBoxType {
    unknown = -1,
    ftyp    = 0,
    mdat,
    moov,
    pnot,
    udta,
    uuid,
    moof,
    free,
    skip,
    jp2,
    wide,
    load,
    ctab,
    imap,
    matt,
    kmat,
    clip,
    crgn,
    sync,
    chap,
    tmcd,
    scpt,
    ssrc,
    pict,
};

struct MetaDataBoxHeader {
    uint32_t size;
    MetaDataBoxType type;
    off64_t offset;
};

struct MetaDataBox_ftyp {
    char subType[4];
};

struct MetaDataBox {
    MetaDataBoxHeader header;
    union {
        MetaDataBox_ftyp ftyp;
    };
};
}

class M4AExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit M4AExtractor(DataSourceBase *source);
    ~M4AExtractor();
    virtual status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }

private:
    status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    std::vector<M4A::MetaDataBox> m_headerVec;
};
