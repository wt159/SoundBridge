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

    virtual std::string toString() { return m_name; }

private:
    std::string getExtensionName(const std::string &filename);

private:
    std::ifstream m_file;
    int64_t m_offset;
    int64_t m_length;
    std::mutex m_lock;
    std::string m_name;
    std::string m_extensionName;
};