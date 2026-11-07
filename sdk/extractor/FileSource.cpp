#include "FileSource.h"
#include "LogWrapper.h"
#include "Utf8Path.h"
#include <fstream>
#include <memory>

#define LOG_TAG "FileSource"

using namespace sdk_utils;

FileSource::FileSource(const char *filename)
#ifdef _WIN32
    : m_fp(nullptr)
#else
    : m_file()
#endif
    , m_offset(0)
    , m_length(-1)
    , m_name("<null>")
    , m_extensionName("<null>")
{
    if (filename) {
        m_name          = std::string("FileSource(") + std::string(filename) + std::string(")");
        m_extensionName = getExtensionName(filename);
    }

    LOG_DEBUG(LOG_TAG, "%s", filename ? filename : "(null)");

#ifdef _WIN32
    // 宽字符路径：与系统代码页无关，非 ANSI 的文件名（中文、日文、emoji）都能打开。
    if (filename && filename[0] != '\0') {
        const std::wstring wide = sdk_utils::toWideString(filename);
        if (!wide.empty()) {
            m_fp = _wfopen(wide.c_str(), L"rb");
        }
    }
    if (m_fp != nullptr) {
        if (_fseeki64(m_fp, 0, SEEK_END) == 0) {
            m_length = static_cast<int64_t>(_ftelli64(m_fp));
            _fseeki64(m_fp, 0, SEEK_SET);
        }
        LOGI("file size: %lld", (long long)m_length);
    } else {
        LOG_ERROR(LOG_TAG, "failed to open file '%s'", filename ? filename : "(null)");
    }
#else
    if (filename && filename[0] != '\0') {
        m_file.open(filename, std::ios::in | std::ios::binary);
    }
    if (m_file.is_open()) {
        m_file.seekg(0, std::ios::end);
        m_length = m_file.tellg();
        m_file.seekg(0, std::ios::beg);
        LOGI("file size: %lld", (long long)m_length);
    } else {
        LOG_ERROR(LOG_TAG, "failed to open file '%s'", filename ? filename : "(null)");
    }
#endif
}

FileSource::~FileSource()
{
#ifdef _WIN32
    if (m_fp != nullptr) {
        std::fclose(m_fp);
        m_fp = nullptr;
    }
#else
    if (m_file.is_open()) {
        m_file.close();
    }
#endif
}

bool FileSource::isOpen() const
{
#ifdef _WIN32
    return m_fp != nullptr;
#else
    return m_file.is_open();
#endif
}

status_t FileSource::initCheck() const
{
    return isOpen() ? OK : NO_INIT;
}

ssize_t FileSource::readAt(off64_t offset, void *data, size_t size)
{
    if (!isOpen()) {
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

#ifdef _WIN32
    if (_fseeki64(m_fp, offset + m_offset, SEEK_SET) != 0) {
        LOG_ERROR(LOG_TAG, "seek to %lld failed", (long long)(offset + m_offset));
        return UNKNOWN_ERROR;
    }

    const size_t got = std::fread(data, 1, size, m_fp);
    if (got != size && std::ferror(m_fp) != 0) {
        LOG_ERROR(LOG_TAG, "read file failed");
        return FAILED_TRANSACTION;
    }

    return static_cast<ssize_t>(got);
#else
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
#endif
}

status_t FileSource::getSize(off64_t *size)
{
    std::lock_guard<std::mutex> lock(m_lock);

    if (!isOpen()) {
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
