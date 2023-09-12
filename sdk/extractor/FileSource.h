#pragma once

#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <fstream>
#include <mutex>
#include <string>

class FileSource : public DataSource, public NonCopyable {
public:
    FileSource(const char *filename);

    virtual ~FileSource();

    virtual status_t initCheck() const;

    virtual ssize_t readAt(off64_t offset, void *data, size_t size);

    virtual status_t getSize(off64_t *size);

    virtual uint32_t flags() { return kIsLocalFileSource; }

    virtual std::string toString() { return mName; }

private:
    std::ifstream mFile;
    int64_t mOffset;
    int64_t mLength;
    std::mutex mLock;
    std::string mName;
};