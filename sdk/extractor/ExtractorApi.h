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


}

#endif // !_EXTRACTOR_API_H_