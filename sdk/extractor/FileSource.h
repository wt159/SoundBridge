#pragma once

#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

class FileSource : public DataSource, public NonCopyable {
public:
    FileSource(const char *filename);

    virtual ~FileSource();

    virtual sdk_utils::status_t initCheck() const;

    virtual ssize_t readAt(off64_t offset, void *data, size_t size);

    virtual sdk_utils::status_t getSize(off64_t *size);

    virtual uint32_t flags() { return kIsLocalFileSource; }

    virtual std::string toString() { return m_name; }

    // 仅供测试（ADR 0010）：把 payload 读取放慢，用来制造稳态欠流。
    // 读点落在解码线程，是对 AGENTS.md「测试钩子绝不落解码线程」的显式例外，
    // 例外边界：只此一处、默认 0（零生产开销）、release/acquire、只对 size >= 4096
    // 的读生效（头部与格式探测的小读不受影响）、不得照抄成通例。
    static void setReadDelayForTest(uint32_t ms);

private:
    std::string getExtensionName(const std::string &filename);

    bool isOpen() const;

private:
    // Windows 上用宽字符路径打开（_wfopen），POSIX 用 std::ifstream。
    // 原因是 std::ifstream 只接受窄字符路径，而 UTF-8 → ANSI(CP_ACP) 的转换在非中文系统上
    // 会把无法映射的字符替换成 '?'，中文名文件因此打不开（2026-11-07 Windows CI 抓到）。
#ifdef _WIN32
    std::FILE *m_fp;
#else
    std::ifstream m_file;
#endif
    int64_t m_offset;
    int64_t m_length;
    std::mutex m_lock;
    std::string m_name;
    std::string m_extensionName;
};
