#pragma once

#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <atomic>
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

    // 规格 `doc/streaming-playback-spec.md` §4.5：租约计数。
    // 构造成功即持有 1 个租约（保持既有「构造即可读」用法）；acquire() 只在 0 → 1 时
    // 真正打开并重求长度，release() 只在归零时关闭。已打开时 acquire() 只自增。
    virtual sdk_utils::status_t acquire();
    virtual void release();
    virtual sdk_utils::status_t openStatus() const { return m_lastOpenStatus; }

    // 进程级句柄预算的观察窗（observability-contract.md §5.1）：计数在 acquire/release
    // 的 0↔1 边界维护（控制线程）。这里不是测试专用钩子——生产路径的两个 L2 gauge
    // 也从同一对计数取值；只有 reset 是测试专用。
    static int openHandleCount();
    static int peakOpenHandleCount();
    static void resetHandleStatsForTest();

    // 仅供测试（ADR 0010）：把 payload 读取放慢，用来制造稳态欠流。
    // 读点落在解码线程，是对 AGENTS.md「测试钩子绝不落解码线程」的显式例外，
    // 例外边界：只此一处、默认 0（零生产开销）、release/acquire、只对 size >= 4096
    // 的读生效（头部与格式探测的小读不受影响）、不得照抄成通例。
    static void setReadDelayForTest(uint32_t ms);

private:
    std::string getExtensionName(const std::string &filename);

    bool isOpen() const;

    // 打开 / 关闭的实体：只在 acquire()/release() 的 0↔1 边界调用，调用方持 m_lock。
    sdk_utils::status_t openInternal();
    void closeInternal();
    static sdk_utils::status_t mapOpenErrno(int err);

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
    mutable std::mutex m_lock; // initCheck() const 也要与 acquire/release 串行化
    std::string m_name;
    std::string m_extensionName;
    // 重开用的原始路径（m_name 是 "FileSource(<path>)" 形式的展示串，不能拿来打开）。
    std::string m_path;
    // 租约计数（规格 §4.5）。只由 acquire()/release() 修改，二者和 readAt 共用 m_lock。
    std::atomic<int> m_openRefs;
    // 最近一次打开失败的分类，供调用点做成因文案（#278）；OK 表示最近一次成功。
    std::atomic<sdk_utils::status_t> m_lastOpenStatus;
};
