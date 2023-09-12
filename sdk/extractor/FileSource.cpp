#include "FileSource.h"
#include "LogWrapper.h"
#include <fstream>
#include <memory>

#define LOG_TAG "FileSource"

FileSource::FileSource(const char *filename)
    : mFile(filename, std::ios::binary)
    , mOffset(0)
    , mLength(-1)
    , mName("<null>")
{
    if (filename) {
        mName = std::string("FileSource(") + std::string(filename) + std::string(")");
    }

    LOG_DEBUG(LOG_TAG, "%s", filename);
    if (mFile.is_open()) {
        mFile.seekg(0, std::ios::end);
        mLength = mFile.tellg();
        mFile.seekg(0, std::ios::beg);
    } else {
        LOG_ERROR(LOG_TAG, "failed to open file '%s'", filename);
    }
}

FileSource::~FileSource()
{
    if (mFile.is_open()) {
        mFile.close();
    }
}

status_t FileSource::initCheck() const
{
    return mFile.is_open() ? OK : NO_INIT;
}

ssize_t FileSource::readAt(off64_t offset, void *data, size_t size)
{
    if (!mFile.is_open()) {
        return NO_INIT;
    }
    std::lock_guard<std::mutex> lock(mLock);

    if (mLength >= 0) {
        if (offset >= mLength) {
            return 0; // read beyond EOF
        }
        uint64_t numAvailable = mLength - offset;
        if ((uint64_t)size > numAvailable) {
            size = numAvailable;
        }
    }

    mFile.seekg(offset + mOffset);
    if (mFile.fail()) {
        LOG_ERROR(LOG_TAG, "seek to %lld failed", (long long)(offset + mOffset));
        return UNKNOWN_ERROR;
    }

    mFile.read((char *)data, size);

    if (!mFile.good()) {
        LOG_ERROR(LOG_TAG, "read file failed");
        return FAILED_TRANSACTION;
    }

    return size;
}

status_t FileSource::getSize(off64_t *size)
{
    std::lock_guard<std::mutex> lock(mLock);

    if (!mFile.is_open()) {
        return NO_INIT;
    }

    *size = mLength;

    return OK;
}