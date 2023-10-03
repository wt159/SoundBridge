#pragma once

#include "AudioCommon.hpp"
#include "ByteUtils.h"
#include "ExtractorApi.h"
#include <unordered_map>

/* adds some convience methods */
class DataSourceHelper {
public:
    explicit DataSourceHelper(CDataSource *csource) { m_source = csource; }

    explicit DataSourceHelper(DataSourceHelper *source) { m_source = source->m_source; }

    virtual ~DataSourceHelper() { }

    virtual ssize_t readAt(off64_t offset, void *data, size_t size)
    {
        return m_source->readAt(m_source->handle, offset, data, size);
    }

    virtual status_t getSize(off64_t *size) { return m_source->getSize(m_source->handle, size); }

    bool getUri(char *uriString, size_t bufferSize)
    {
        return m_source->getUri(m_source->handle, uriString, bufferSize);
    }

    virtual uint32_t flags() { return m_source->flags(m_source->handle); }

    // Convenience methods:
    bool getUInt16(off64_t offset, uint16_t *x)
    {
        *x = 0;

        uint8_t byte[2];
        if (readAt(offset, byte, 2) != 2) {
            return false;
        }

        *x = (byte[0] << 8) | byte[1];

        return true;
    }

    // 3 byte int, returned as a 32-bit int
    bool getUInt24(off64_t offset, uint32_t *x)
    {
        *x = 0;

        uint8_t byte[3];
        if (readAt(offset, byte, 3) != 3) {
            return false;
        }

        *x = (byte[0] << 16) | (byte[1] << 8) | byte[2];

        return true;
    }

    bool getUInt32(off64_t offset, uint32_t *x)
    {
        *x = 0;

        uint32_t tmp;
        if (readAt(offset, &tmp, 4) != 4) {
            return false;
        }

        *x = sdk_utils::ntohl(tmp);

        return true;
    }

    bool getUInt64(off64_t offset, uint64_t *x)
    {
        *x = 0;

        uint64_t tmp;
        if (readAt(offset, &tmp, 8) != 8) {
            return false;
        }

        *x = ((uint64_t)sdk_utils::ntohl(tmp & 0xffffffff) << 32) | sdk_utils::ntohl(tmp >> 32);

        return true;
    }

    // read either int<N> or int<2N> into a uint<2N>_t, size is the int size in bytes.
    bool getUInt16Var(off64_t offset, uint16_t *x, size_t size)
    {
        if (size == 2) {
            return getUInt16(offset, x);
        }
        if (size == 1) {
            uint8_t tmp;
            if (readAt(offset, &tmp, 1) == 1) {
                *x = tmp;
                return true;
            }
        }
        return false;
    }

    bool getUInt32Var(off64_t offset, uint32_t *x, size_t size)
    {
        if (size == 4) {
            return getUInt32(offset, x);
        }
        if (size == 2) {
            uint16_t tmp;
            if (getUInt16(offset, &tmp)) {
                *x = tmp;
                return true;
            }
        }
        return false;
    }

    bool getUInt64Var(off64_t offset, uint64_t *x, size_t size)
    {
        if (size == 8) {
            return getUInt64(offset, x);
        }
        if (size == 4) {
            uint32_t tmp;
            if (getUInt32(offset, &tmp)) {
                *x = tmp;
                return true;
            }
        }
        return false;
    }

protected:
    CDataSource *m_source;
};

enum standardExtractors {
    WAV_EXTRACTOR,
    MP3_EXTRACTOR,
    AAC_EXTRACTOR,
    FLAC_EXTRACTOR,
    OGG_EXTRACTOR,
    AIFF_EXTRACTOR,
    ASF_EXTRACTOR,
    M4A_EXTRACTOR,
    OPUS_EXTRACTOR,
    UNKNOWN_EXTRACTOR
};

static const std::unordered_map<std::string, standardExtractors> defaultExtractorMap = {
    { ".wav", WAV_EXTRACTOR },
    /* { ".mp3", MP3_EXTRACTOR }, { ".aac", AAC_EXTRACTOR },
{ ".flac", FLAC_EXTRACTOR }, { ".ogg", OGG_EXTRACTOR }, { ".aiff", AIFF_EXTRACTOR },
{ ".asf", ASF_EXTRACTOR },   { ".m4a", M4A_EXTRACTOR }, { ".opus", OPUS_EXTRACTOR } */
};

class ExtractorHelper {
public:
    virtual ~ExtractorHelper() { }

    virtual const char *name() { return "<unspecified>"; }

    virtual uint64_t getDurationMs()                                      = 0;
    virtual off64_t getDataSize()                                         = 0;
    virtual AudioSpec getAudioSpec()                                      = 0;
    virtual void readAudioRawData(off64_t offset, size_t size, void *buf) = 0;

protected:
    ExtractorHelper() { }

private:
    ExtractorHelper(const ExtractorHelper &);
    ExtractorHelper &operator=(const ExtractorHelper &);
};