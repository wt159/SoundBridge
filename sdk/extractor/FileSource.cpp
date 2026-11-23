#include "FileSource.h"
#include "LogWrapper.h"
#include "Utf8Path.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

#define LOG_TAG "FileSource"

using namespace sdk_utils;

namespace {
// 仅供测试（ADR 0010）：见 FileSource.h 的例外边界。默认 0，生产路径零开销。
std::atomic<uint32_t> g_readDelayMs(0);

// 仅供测试（ADR 0011 / 地图 #285）：注入 open 延迟，模拟慢介质上的切歌重开。
// 与 g_readDelayMs 同一纪律：默认 0、只影响 acquire() 的调用线程、测完必须归零。
std::atomic<uint32_t> g_openDelayMs(0);

// 进程级句柄预算（规格 observability-contract.md §5.1，L2 只记录不设阈值）。
// 维护点只在 acquire/release 的 0↔1 边界与析构，全部发生在控制线程。
std::atomic<int> g_openHandles(0);
std::atomic<int> g_peakHandles(0);
} // namespace

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
    , m_path()
    , m_openRefs(0)
    , m_lastOpenStatus(sdk_utils::OK)
{
    if (filename) {
        m_path          = std::string(filename);
        m_name          = std::string("FileSource(") + std::string(filename) + std::string(")");
        m_extensionName = getExtensionName(filename);
    }

    LOG_DEBUG(LOG_TAG, "%s", filename ? filename : "(null)");

    // 构造即持有一个租约（规格 streaming-playback-spec.md §4.5「构造租约」）：
    // 保持既有「构造即可读」的直连用法不变；探测路径在 getAudioSpec() 之后 release()，
    // 计数归零即关闭，条目不再常驻句柄。
    std::lock_guard<std::mutex> lock(m_lock);
    if (openInternal() == OK) {
        m_openRefs.store(1);
    }
}

FileSource::~FileSource()
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_openRefs.load() > 0 || isOpen()) {
        closeInternal();
    }
    m_openRefs.store(0);
}

status_t FileSource::openInternal()
{
    if (isOpen()) {
        m_lastOpenStatus.store(OK);
        return OK;
    }
    if (m_path.empty()) {
        m_lastOpenStatus.store(NO_INIT);
        return NO_INIT;
    }

#ifdef _WIN32
    // 宽字符路径：与系统代码页无关，非 ANSI 的文件名（中文、日文、emoji）都能打开。
    const std::wstring wide = sdk_utils::toWideString(m_path.c_str());
    if (wide.empty()) {
        m_lastOpenStatus.store(NO_INIT);
        return NO_INIT;
    }
    errno = 0;
    m_fp  = _wfopen(wide.c_str(), L"rb");
    if (m_fp == nullptr) {
        const status_t status = mapOpenErrno(errno);
        m_lastOpenStatus.store(status);
        LOG_ERROR(LOG_TAG, "failed to open file '%s' (errno=%d)", m_path.c_str(), errno);
        return status;
    }
    m_length = -1;
    if (_fseeki64(m_fp, 0, SEEK_END) == 0) {
        m_length = static_cast<int64_t>(_ftelli64(m_fp));
        _fseeki64(m_fp, 0, SEEK_SET);
    }
#else
    errno = 0;
    // 重开时必须先清掉上一次失败留下的 failbit，否则 open() 直接被忽略。
    m_file.clear();
    m_file.open(m_path.c_str(), std::ios::in | std::ios::binary);
    if (!m_file.is_open()) {
        const status_t status = mapOpenErrno(errno);
        m_lastOpenStatus.store(status);
        LOG_ERROR(LOG_TAG, "failed to open file '%s' (errno=%d)", m_path.c_str(), errno);
        m_file.clear();
        return status;
    }
    m_length = -1;
    m_file.seekg(0, std::ios::end);
    m_length = static_cast<int64_t>(m_file.tellg());
    m_file.seekg(0, std::ios::beg);
#endif

    LOGI("file size: %lld", (long long)m_length);
    m_lastOpenStatus.store(OK);

    const int now  = g_openHandles.fetch_add(1) + 1;
    int peak       = g_peakHandles.load(std::memory_order_acquire);
    while (now > peak
           && !g_peakHandles.compare_exchange_weak(peak, now, std::memory_order_release,
                                                  std::memory_order_acquire)) {
    }
    return OK;
}

void FileSource::closeInternal()
{
#ifdef _WIN32
    if (m_fp != nullptr) {
        std::fclose(m_fp);
        m_fp = nullptr;
        g_openHandles.fetch_sub(1);
    }
#else
    if (m_file.is_open()) {
        m_file.close();
        m_file.clear();
        g_openHandles.fetch_sub(1);
    }
#endif
    m_length = -1;
}

status_t FileSource::mapOpenErrno(int err)
{
    switch (err) {
#ifdef EMFILE
    case EMFILE:
#endif
#ifdef ENFILE
    case ENFILE:
#endif
    case ENOMEM:
        return NO_MEMORY;
#ifdef EACCES
    case EACCES:
#endif
    case EPERM:
        return PERMISSION_DENIED;
#ifdef ENOENT
    case ENOENT:
#endif
#ifdef ENOTDIR
    case ENOTDIR:
#endif
        return NO_INIT;
    default:
        return UNKNOWN_ERROR;
    }
}

status_t FileSource::acquire()
{
    // 仅供测试（#285 / ADR 0020）：慢介质模拟。延迟在拿锁之前——卡住的 open
    // 本来就不该影响别的读者（真实场景里读的是已打开句柄）。
    const uint32_t openDelayMs = g_openDelayMs.load(std::memory_order_acquire);
    if (openDelayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(openDelayMs));
    }
    // 与 readAt 共用 m_lock：控制线程打开/关闭时不会撕裂正在进行的读。
    // 关闭晚于 join 由租约与声明顺序保证（规格 §4.5 不变量 3/4），这里只做串行化。
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_openRefs.load() == 0) {
        const status_t status = openInternal();
        if (status != OK) {
            return status;
        }
        m_openRefs.store(1);
        return OK;
    }
    m_openRefs.fetch_add(1);
    return OK;
}

void FileSource::release()
{
    std::lock_guard<std::mutex> lock(m_lock);
    const int current = m_openRefs.load();
    if (current <= 0) {
        // 防御：不配对的 release 不得把计数打成负数，也不得关掉别人正在用的句柄。
        return;
    }
    if (current == 1) {
        closeInternal();
        m_openRefs.store(0);
        return;
    }
    m_openRefs.fetch_sub(1);
}

int FileSource::openHandleCount()
{
    return g_openHandles.load();
}

int FileSource::peakOpenHandleCount()
{
    return g_peakHandles.load();
}

void FileSource::resetHandleStatsForTest()
{
    // 采样当前值而不是清零：别的测试可能仍持有句柄，峰值必须以现状为基线。
    g_peakHandles.store(g_openHandles.load());
}

void FileSource::setReadDelayForTest(uint32_t ms)
{
    g_readDelayMs.store(ms, std::memory_order_release);
}

void FileSource::setOpenDelayForTest(uint32_t ms)
{
    g_openDelayMs.store(ms, std::memory_order_release);
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
    // 与 acquire/release 同锁：控制线程重开时，其他线程的 initCheck 不会读到半开状态。
    std::lock_guard<std::mutex> lock(m_lock);
    return isOpen() ? OK : NO_INIT;
}

ssize_t FileSource::readAt(off64_t offset, void *data, size_t size)
{
    // 关闭检查放进临界区：acquire/release 会在控制线程写 m_fp / m_file，锁外读是数据竞争
    // （code-review 2026-11-22）。正确性仍由租约与 join 次序保证，这里只消除竞争本身。
    std::lock_guard<std::mutex> lock(m_lock);
    if (!isOpen()) {
        return NO_INIT;
    }

    if (m_length >= 0) {
        if (offset >= m_length) {
            return 0; // read beyond EOF
        }
        uint64_t numAvailable = m_length - offset;
        if ((uint64_t)size > numAvailable) {
            size = numAvailable;
        }
    }

    // 仅供测试（ADR 0010）：放慢 payload 读取，制造稳态欠流。只对 >= 4096 B 的读生效，
    // 头部与格式探测的 8~70 B 小读保持原速。
    const uint32_t delayMs = g_readDelayMs.load(std::memory_order_acquire);
    if (delayMs > 0 && size >= 4096) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
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
