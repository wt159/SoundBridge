#include "FileSource.h"
#include "LogWrapper.h"
#include <fstream>
#include <memory>

#define LOG_TAG "FileSource"

FileSource::FileSource(const char *filename)
    : m_file(filename, std::ios::binary)
    , m_offset(0)
    , m_length(-1)
    , m_name("<null>")
    , m_extensionName("<null>")
{
    if (filename) {
        m_name = std::string("FileSource(") + std::string(filename) + std::string(")");
        m_extensionName = getExtensionName(filename);
    }

    LOG_DEBUG(LOG_TAG, "%s", filename);
    if (m_file.is_open()) {
        m_file.seekg(0, std::ios::end);
        m_length = m_file.tellg();
        m_file.seekg(0, std::ios::beg);
        LOGI("file size: %lld", (long long)m_length);
    } else {
        LOG_ERROR(LOG_TAG, "failed to open file '%s'", filename);
    }
}

FileSource::~FileSource()
{
    if (m_file.is_open()) {
        m_file.close();
    }
}

status_t FileSource::initCheck() const
{
    return m_file.is_open() ? OK : NO_INIT;
}

ssize_t FileSource::readAt(off64_t offset, void *data, size_t size)
{
    if (!m_file.is_open()) {
        return NO_INIT;
    }
    std::lock_guard<std::mutex> lock(m_lock);

    if (m_length >= 0) {
        if (offset >= m_length) {
            return 0; // read beyond EOF
        }
        uint64_t numAvailable = m_length - offset;
        if ((uint64_t)size > numAvailable) {
            size = numAvailable;
        }
    }

    m_file.seekg(offset + m_offset);
    if (m_file.fail()) {
        LOG_ERROR(LOG_TAG, "seek to %lld failed", (long long)(offset + m_offset));
        return UNKNOWN_ERROR;
    }

    m_file.read((char *)data, size);

    if (!m_file.good()) {
        LOG_ERROR(LOG_TAG, "read file failed");
        return FAILED_TRANSACTION;
    }

    return size;
}

status_t FileSource::getSize(off64_t *size)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!m_file.is_open()) {
        return NO_INIT;
    }

    *size = m_length;

    return OK;
}

std::string FileSource::getExtensionName(const std::string &filename)
{
    size_t pos = filename.rfind('.');
    if (pos != std::string::npos && pos < filename.length() - 1) {
        return filename.substr(pos + 1);
    }
    return std::string();
}
