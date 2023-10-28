#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <memory>
#include <string>
#include <vector>

namespace AIFF {

enum {
    kChunkID_FORM = 0x464F524D, /* 'FORM' */
    kChunkID_COMM = 0x434F4D4D, /* 'COMM' */
    kChunkID_SSND = 0x53534E44, /* 'SSND' */
    kChunkID_FVER = 0x46564552, /* 'FVER' */
};

struct Chunk {
    uint32_t chunkID;
    uint32_t chunkSize;
};

using ChunkPtr = std::shared_ptr<Chunk>;

struct FormAiffChunk : public Chunk {
    /* ID   'FORM' */
    /* size fileSize - 8 */
    uint32_t formType; /* AIFC */
    std::vector<ChunkPtr> chunkVec;
};

struct FormatVersionChunk : public Chunk {
    /* ID   'FVER' */
    /* size 4 */
    uint32_t timestamp; /* AIFC Version 1 */
};

struct CommonChunk : public Chunk {
    /* ID 'COMM' */
    /* size 30 + name.len() */
    uint16_t numChannels;
    uint32_t numSampleFrames;
    uint16_t sampleSize;
    uint32_t sampleRate;    /* 80bits, 浮点 */
};

struct SoundDataChunk : public Chunk {
    uint32_t offset;
    uint32_t blockSize;
    AudioBuffer::AudioBufferPtr soundData;
};

}

class AIFFExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit AIFFExtractor(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual ~AIFFExtractor();

private:
    sdk_utils::status_t init();
    void commonChunk2AudioSpec(const AIFF::CommonChunk *cChunk, AudioSpec &audioSpec);

private:
    DataSourceBase *m_dataSource;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    AIFF::FormAiffChunk m_formChunk;
};