#ifndef _EXTRACTOR_API_H_
#define _EXTRACTOR_API_H_

#include "Errors.h"
#include <stdint.h>
#include <sys/types.h>

extern "C" {

enum {
    MEDIA_ERROR_BASE        = -1000,
    ERROR_ALREADY_CONNECTED = MEDIA_ERROR_BASE,
    ERROR_NOT_CONNECTED     = MEDIA_ERROR_BASE - 1,
    ERROR_UNKNOWN_HOST      = MEDIA_ERROR_BASE - 2,
    ERROR_CANNOT_CONNECT    = MEDIA_ERROR_BASE - 3,
    ERROR_IO                = MEDIA_ERROR_BASE - 4,
    ERROR_CONNECTION_LOST   = MEDIA_ERROR_BASE - 5,
    ERROR_MALFORMED         = MEDIA_ERROR_BASE - 7,
    ERROR_OUT_OF_RANGE      = MEDIA_ERROR_BASE - 8,
    ERROR_BUFFER_TOO_SMALL  = MEDIA_ERROR_BASE - 9,
    ERROR_UNSUPPORTED       = MEDIA_ERROR_BASE - 10,
    ERROR_END_OF_STREAM     = MEDIA_ERROR_BASE - 11,
};

struct CDataSource {
    ssize_t (*readAt)(void *handle, off64_t offset, void *data, size_t size);
    status_t (*getSize)(void *handle, off64_t *size);
    uint32_t (*flags)(void *handle);
    bool (*getUri)(void *handle, char *uriString, size_t bufferSize);
    void *handle;
};

enum CMediaTrackReadOptions : uint32_t {
    SEEK_PREVIOUS_SYNC = 0,
    SEEK_NEXT_SYNC     = 1,
    SEEK_CLOSEST_SYNC  = 2,
    SEEK_CLOSEST       = 3,
    SEEK_FRAME_INDEX   = 4,
    SEEK               = 8,
    NONBLOCKING        = 16
};

/**
 * only use CMediaBuffer allocated from the CMediaBufferGroup that is
 * provided to CMediaTrack::start()
 */
struct CMediaBuffer {
    void *handle;
    void (*release)(void *handle);
    void *(*data)(void *handle);
    size_t (*size)(void *handle);
    size_t (*range_offset)(void *handle);
    size_t (*range_length)(void *handle);
    void (*set_range)(void *handle, size_t offset, size_t length);
    //  AMediaFormat* (*meta_data)(void *handle);
};

struct CMediaBufferGroup {
    void *handle;
    bool (*init)(void *handle, size_t buffers, size_t buffer_size, size_t growthLimit);
    void (*add_buffer)(void *handle, size_t size);
    //  media_status_t (*acquire_buffer)(void *handle,
    //  CMediaBuffer **buffer, bool nonBlocking, size_t requestedSize);
    bool (*has_buffers)(void *handle);
};

struct CMediaTrack {
    void *data;
    void (*free)(void *data);

    //  media_status_t (*start)(void *data, CMediaBufferGroup *bufferGroup);
    //  media_status_t (*stop)(void *data);
    //  media_status_t (*getFormat)(void *data, AMediaFormat *format);
    //  media_status_t (*read)(void *data, CMediaBuffer **buffer, uint32_t options, int64_t
    //  seekPosUs);
    bool (*supportsNonBlockingRead)(void *data);
};

struct CMediaExtractor {
    void *data;

    void (*free)(void *data);
    size_t (*countTracks)(void *data);
    CMediaTrack *(*getTrack)(void *data, size_t index);
    //  media_status_t (*getTrackMetaData)(
    //          void *data,
    //          AMediaFormat *meta,
    //          size_t index, uint32_t flags);

    //  media_status_t (*getMetaData)(void *data, AMediaFormat *meta);
    uint32_t (*flags)(void *data);
    //  media_status_t (*setMediaCas)(void *data, const uint8_t* casToken, size_t size);
    const char *(*name)(void *data);
};

typedef CMediaExtractor *(*CreatorFunc)(CDataSource *source, void *meta);
typedef void (*FreeMetaFunc)(void *meta);

// The sniffer can optionally fill in an opaque object, "meta", that helps
// the corresponding extractor initialize its state without duplicating
// effort already exerted by the sniffer. If "freeMeta" is given, it will be
// called against the opaque object when it is no longer used.
typedef CreatorFunc (*SnifferFunc)(
    CDataSource *source, float *confidence, void **meta, FreeMetaFunc *freeMeta);

typedef CMediaExtractor CMediaExtractor;
typedef CreatorFunc CreatorFunc;

typedef struct {
    const uint8_t b[16];
} media_uuid_t;

struct ExtractorDef {
    // version number of this structure
    const uint32_t def_version;

    // A unique identifier for this extractor.
    // See below for a convenience macro to create this from a string.
    media_uuid_t extractor_uuid;

    // Version number of this extractor. When two extractors with the same
    // uuid are encountered, the one with the largest version number will
    // be used.
    const uint32_t extractor_version;

    // a human readable name
    const char *extractor_name;

    union {
        struct {
            SnifferFunc sniff;
        } v2;
        struct {
            SnifferFunc sniff;
            // a NULL terminated list of container mime types and/or file extensions
            // that this extractor supports
            const char **supported_types;
        } v3;
    } u;
};

// the C++ based API which first shipped in P and is no longer supported
const uint32_t EXTRACTORDEF_VERSION_LEGACY = 1;

// the first C/NDK based API
const uint32_t EXTRACTORDEF_VERSION_NDK_V1 = 2;

// the second C/NDK based API
const uint32_t EXTRACTORDEF_VERSION_NDK_V2 = 3;

const uint32_t EXTRACTORDEF_VERSION = EXTRACTORDEF_VERSION_NDK_V2;

// each plugin library exports one function of this type
typedef ExtractorDef (*GetExtractorDef)();
}

#endif // !_EXTRACTOR_API_H_