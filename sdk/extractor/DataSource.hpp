#pragma once

#include "ByteUtils.h"
#include "ErrorUtils.h"
#include <string>

class DataSourceBase {
public:
    enum Flags {
        kWantsPrefetching      = 1,
        kStreamedFromLocalHost = 2,
        kIsCachingDataSource   = 4,
        kIsHTTPBasedSource     = 8,
        kIsLocalFileSource     = 16,
    };

    DataSourceBase() { }

    virtual sdk_utils::status_t initCheck() const = 0;

    // Returns the number of bytes read, or -1 on failure. It's not an error if
    // this returns zero; it just means the given offset is equal to, or
    // beyond, the end of the source.
    virtual ssize_t readAt(off64_t offset, void *data, size_t size) = 0;

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

        *x = sdk_utils::ntoh64(tmp);

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

    virtual sdk_utils::status_t getSize(off64_t *size)
    {
        *size = 0;
        return sdk_utils::UNKNOWN_ERROR;
    }

    virtual bool getUri(char * /*uriString*/, size_t /*bufferSize*/) { return false; }

    virtual uint32_t flags() { return 0; }

    virtual void close() {};

    virtual sdk_utils::status_t getAvailableSize(off64_t /*offset*/, off64_t * /*size*/) { return -1; }

protected:
    virtual ~DataSourceBase() { }

private:
    DataSourceBase(const DataSourceBase &);
    DataSourceBase &operator=(const DataSourceBase &);
};

class DataSource : public DataSourceBase {
public:
    DataSource() { }

    virtual ~DataSource() { }

    virtual std::string toString() { return std::string("<unspecified>"); }

    virtual sdk_utils::status_t reconnectAtOffset(off64_t /*offset*/) { return sdk_utils::ERROR_UNSUPPORTED; }

    ////////////////////////////////////////////////////////////////////////////

    virtual std::string getUri() { return std::string(); }

    virtual bool getUri(char *uriString, size_t bufferSize) final
    {
        int ret = snprintf(uriString, bufferSize, "%s", getUri().c_str());
        return ret >= 0 && static_cast<size_t>(ret) < bufferSize;
    }

private:
    DataSource(const DataSource &);
    DataSource &operator=(const DataSource &);
};