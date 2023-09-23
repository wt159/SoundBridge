#pragma once

#include "ByteUtils.hpp"
#include "ExtractorApi.h"

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

class ExtractorHelper {
public:
    virtual ~ExtractorHelper() { }
    // virtual size_t countTracks() = 0;
    // virtual MediaTrackHelper *getTrack(size_t index) = 0;

    enum GetTrackMetaDataFlags { kIncludeExtensiveMetaData = 1 };
    // virtual media_status_t getTrackMetaData(AMediaFormat *meta, size_t index, uint32_t flags = 0)
    //     = 0;

    // Return container specific meta-data. The default implementation
    // returns an empty metadata object.
    // virtual media_status_t getMetaData(AMediaFormat *meta) = 0;

    enum Flags {
        CAN_SEEK_BACKWARD = 1, // the "seek 10secs back button"
        CAN_SEEK_FORWARD  = 2, // the "seek 10secs forward button"
        CAN_PAUSE         = 4,
        CAN_SEEK          = 8, // the "seek bar"
    };

    // If subclasses do _not_ override this, the default is
    // CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK | CAN_PAUSE
    // virtual uint32_t flags() const
    // {
    //     return CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK | CAN_PAUSE;
    // };

    // virtual media_status_t setMediaCas(const uint8_t * /*casToken*/, size_t /*size*/)
    // {
    //     return AMEDIA_ERROR_INVALID_OPERATION;
    // }

    virtual const char *name() { return "<unspecified>"; }

protected:
    ExtractorHelper() { }

private:
    ExtractorHelper(const ExtractorHelper &);
    ExtractorHelper &operator=(const ExtractorHelper &);
};