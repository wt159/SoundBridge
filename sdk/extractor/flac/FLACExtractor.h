#pragma once

#include "AudioBuffer.h"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <vector>

// 这是 FLAC 比特流的概述。

// fLaC 标记：此标记被添加到流的开头。它后面是一个或多个元数据块。

// 元数据块：FLAC支持128种元数据块；目前定义了以下内容。

//     STREAMINFO: Contains the information about the whole stream.
//     APPLICATION: This is used by third-party applications for identification.
//     PADDING: It is used to reserve space for metadata if the metadata will be edited after
//     encoding. When the metadata is edited, the padding is replaced by the actual metadata.
//     SEEKTABLE: An optional table to store seek points.
//     VORBIS_COMMENT: Used to store human-readable key/value pairs.
//     CUESHEET: Used to store cue sheet information.
//     PICTURE: Used to store pictures.
// FRAME：音频数据由一个或多个音频帧组成。

//     FRAME_HEADER: Contains the basic information about the stream.
//     SUBFRAME: To decrease the complexity, individual subframes are coded separately within a
//     frame (one frame per channel). FRAME_FOOTER: Contains the CRC of the complete frame.

struct FileMark {
    uint8_t id[4]; /* fLaC */
};

enum MetaDataBlockType {
    STREAMINFO     = 0,
    PADDING        = 1,
    APPLICATION    = 2,
    SEEKTABLE      = 3,
    VORBIS_COMMENT = 4,
    CUESHEET       = 5,
    PICTURE        = 6,
    UNKNOWN        = 127
};

struct MetaDataBlockHeader {
    uint8_t isLastBlock : 1;
    uint8_t type        : 7;
    uint32_t blockSize  : 24;
};

struct MetaDataBlock_StreamInfo {
    uint16_t minBlockSize;
    uint16_t maxBlockSize;
    uint32_t minFrameSize;
    uint32_t maxFrameSize;
    uint32_t sampleRate;
    uint8_t channels;
    uint8_t bitsPerSample;
    uint64_t totalSamples;
    uint8_t md5[16];
};

struct MetaDataBlock_Padding {
    uint8_t dummy;
};

struct MetaDataBlock_Application {
    uint8_t id[4];
    uint8_t *data;
};

struct MetaData_SeekPoint {
    uint64_t sampleNumber;
    uint64_t streamOffset;
    uint32_t frameSamples;
};

struct MetaDataBlock_SeekTable {
    uint32_t numPoints;
    MetaData_SeekPoint *points;
};

struct MetaData_VorbisCommentEntry {
    uint32_t length;
    uint8_t *entry;
};

struct MetaDataBlock_VorbisComment {
    MetaData_VorbisCommentEntry vendorString;
    uint32_t numComments;
    MetaData_VorbisCommentEntry *comments;
};

struct MetaData_CurSheetIndex {
    uint64_t offset;
    uint64_t number;
};

struct Metadata_CueSheetTrack {
    uint64_t offset;
    uint8_t number;
    char isrc[13];
    uint32_t type         : 1;
    uint32_t pre_emphasis : 1;
    uint8_t num_indices;
    MetaData_CurSheetIndex *indices;
};

struct MetaDataBlock_CueSheet {
    char mediaCatalogNumber[128];
    uint64_t leadInSamples;
    bool isCd;
    uint8_t numTracks;
    Metadata_CueSheetTrack *tracks;
};

enum MetaData_PictureType {
    METADATA_PICTURE_TYPE_OTHER                = 0, /**< Other */
    METADATA_PICTURE_TYPE_FILE_ICON_STANDARD   = 1, /**< 32x32 pixels 'file icon' (PNG only) */
    METADATA_PICTURE_TYPE_FILE_ICON            = 2, /**< Other file icon */
    METADATA_PICTURE_TYPE_FRONT_COVER          = 3, /**< Cover (front) */
    METADATA_PICTURE_TYPE_BACK_COVER           = 4, /**< Cover (back) */
    METADATA_PICTURE_TYPE_LEAFLET_PAGE         = 5, /**< Leaflet page */
    METADATA_PICTURE_TYPE_MEDIA                = 6, /**< Media (e.g. label side of CD) */
    METADATA_PICTURE_TYPE_LEAD_ARTIST          = 7, /**< Lead artist/lead performer/soloist */
    METADATA_PICTURE_TYPE_ARTIST               = 8, /**< Artist/performer */
    METADATA_PICTURE_TYPE_CONDUCTOR            = 9, /**< Conductor */
    METADATA_PICTURE_TYPE_BAND                 = 10, /**< Band/Orchestra */
    METADATA_PICTURE_TYPE_COMPOSER             = 11, /**< Composer */
    METADATA_PICTURE_TYPE_LYRICIST             = 12, /**< Lyricist/text writer */
    METADATA_PICTURE_TYPE_RECORDING_LOCATION   = 13, /**< Recording Location */
    METADATA_PICTURE_TYPE_DURING_RECORDING     = 14, /**< During recording */
    METADATA_PICTURE_TYPE_DURING_PERFORMANCE   = 15, /**< During performance */
    METADATA_PICTURE_TYPE_VIDEO_SCREEN_CAPTURE = 16, /**< Movie/video screen capture */
    METADATA_PICTURE_TYPE_FISH                 = 17, /**< A bright coloured fish */
    METADATA_PICTURE_TYPE_ILLUSTRATION         = 18, /**< Illustration */
    METADATA_PICTURE_TYPE_BAND_LOGOTYPE        = 19, /**< Band/artist logotype */
    METADATA_PICTURE_TYPE_PUBLISHER_LOGOTYPE   = 20, /**< Publisher/Studio logotype */
    METADATA_PICTURE_TYPE_UNDEFINED
};

struct MetaDataBlock_Picture {
    MetaData_PictureType type;
    char *mimeType;
    uint8_t *description;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t numColors;
    uint32_t dataLength;
    uint8_t *data;
};

struct MetaDataBlock_Unknown {
    uint8_t data[0];
};

struct MetaDataBlock {
    MetaDataBlockHeader header;
    union {
        MetaDataBlock_StreamInfo streamInfo;
        MetaDataBlock_Padding padding;
        MetaDataBlock_Application application;
        MetaDataBlock_SeekTable seekTable;
        MetaDataBlock_VorbisComment vorbisComment;
        MetaDataBlock_CueSheet cueSheet;
        MetaDataBlock_Picture picture;
        MetaDataBlock_Unknown unknown;
    } data;
};

enum AudioFrameHeaderBlockingStrategy {
    FIXED_BLOCKING_STRATEGY    = 0,
    VARIABLE_BLOCKING_STRATEGY = 1
};

struct AudioFrameHeader {
    uint16_t syncCode         : 14;
    uint8_t blockingReserved  : 1;
    uint8_t blockingStrategy  : 1;
    uint8_t samples           : 4;
    uint8_t sampleRate        : 4;
    uint8_t channelAssignment : 4;
    uint8_t bitsPerSample     : 3;
    uint8_t reserved          : 1;
    uint8_t sampleRateCode    : 4;
    uint8_t frameNumber[2];
    uint8_t sampleNumber[3];
    uint8_t blockSize[2];
    uint8_t *data;
    uint8_t crc8;
};

struct AudioSubFrame {
    uint8_t type;
    uint8_t wastedBits;
    uint8_t *data;
};

struct AudioFrameFooter {
    uint16_t crc16;
};

struct AudioFrame {
    AudioFrameHeader header;
    std::vector<AudioSubFrame> subFrameVec;
    AudioFrameFooter footer;
};

struct FLACHeader {
    FileMark fileMark;
    std::vector<MetaDataBlock> metaDataBlockVec;
    std::vector<AudioFrame> audioFrameVec;
};

class FLACExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit FLACExtractor(DataSourceBase *source);
    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_spec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getMetaData() { return m_metaBuf; }
    virtual ~FLACExtractor();

private:
    sdk_utils::status_t init();

private:
    DataSourceBase *m_dataSource;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_metaBuf;
    sdk_utils::status_t m_initCheck;
    bool m_validFormat;
    AudioSpec m_spec;
    FLACHeader m_header;
};