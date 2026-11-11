#pragma once

#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <mutex>
#include <string>

// P0 静态 HTTP 点播数据源：把 Range: bytes=<off>-<off+size-1> 映射为
// DataSource::readAt(off, buf, size)。只接 http://；清单类 / https 明确拒绝。
// 所有 socket / HTTP 细节封在 HttpSource.cpp 内，头文件不暴露平台类型。
class HttpSource : public DataSource, public NonCopyable {
public:
    HttpSource(const char *url);
    virtual ~HttpSource();

    virtual sdk_utils::status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual sdk_utils::status_t getSize(off64_t *size);
    virtual uint32_t flags() { return kIsHTTPBasedSource; }
    virtual std::string toString() { return m_name; }
    virtual void close();

private:
    bool probe();

    std::string m_name;
    std::string m_host;
    std::string m_port;
    std::string m_path;
    off64_t m_total;
    sdk_utils::status_t m_initCheck;
    bool m_wsaHeld;
    std::mutex m_lock;
};
