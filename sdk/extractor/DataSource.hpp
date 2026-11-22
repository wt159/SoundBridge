#pragma once

#include "ByteUtils.h"
#include "ErrorUtils.h"
#include <atomic>
#include <memory>
#include <string>
#include <utility>

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

    // 内部取消注入口：注入的是 AudioStreamDecoder::m_abortDecode 这唯一信号的地址，
    // 不新建信号、不是能力查询、不进公开面。flag 为空表示尚未注入（init 期扫描）。
    virtual void setAbortFlag(const std::atomic<bool> *flag)
    {
        m_abortFlag.store(flag, std::memory_order_release);
    }
    virtual bool abortRequested() const
    {
        const std::atomic<bool> *flag = m_abortFlag.load(std::memory_order_acquire);
        return flag != nullptr && flag->load(std::memory_order_acquire);
    }

    virtual void close() {};

    // 规格 `doc/streaming-playback-spec.md` §4.5：本地文件源的「可关闭、可重开、按租约存活」契约。
    // 默认实现保持网络源现状——acquire() 退化为 initCheck()、release() 不做任何事，
    // 因此网络源的连接生命周期（HttpSource::close() 等）一字不动。
    virtual sdk_utils::status_t acquire() { return initCheck(); }
    virtual void release() { }

    // 诊断用：最近一次打开失败的内部分类。默认不表态，调用点退化为通用文案；
    // FileSource 用它区分「句柄耗尽 / 权限 / 找不到」三种成因（#278）。
    virtual sdk_utils::status_t openStatus() const { return sdk_utils::UNKNOWN_ERROR; }

    virtual sdk_utils::status_t getAvailableSize(off64_t /*offset*/, off64_t * /*size*/)
    {
        return -1;
    }

protected:
    std::atomic<const std::atomic<bool> *> m_abortFlag{nullptr};

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

    virtual sdk_utils::status_t reconnectAtOffset(off64_t /*offset*/)
    {
        return sdk_utils::ERROR_UNSUPPORTED;
    }

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

// 一次「数据源此刻可读」的持有凭证（规格 §4.5）：
// 构造时 acquire()，析构时 release()。PlaybackTask 持有它，声明顺序保证解码线程
// join 之后才释放；重播同一曲时两个租约使计数为 2，旧任务释放不会关掉新任务的句柄。
// acquire() 失败时对象仍可存在，但析构不再 release（避免不配对释放）。
class SourceLease {
public:
    SourceLease() { }

    explicit SourceLease(const std::shared_ptr<DataSource> &source)
        : m_source(source)
    {
        if (m_source == nullptr) {
            m_status = sdk_utils::UNEXPECTED_NULL;
            return;
        }
        m_status   = m_source->acquire();
        m_acquired = (m_status == sdk_utils::OK);
    }

    SourceLease(SourceLease &&other)
        : m_source(std::move(other.m_source))
        , m_status(other.m_status)
        , m_acquired(other.m_acquired)
    {
        other.m_status   = sdk_utils::OK;
        other.m_acquired = false;
    }

    SourceLease &operator=(SourceLease &&other)
    {
        if (this != &other) {
            reset();
            m_source         = std::move(other.m_source);
            m_status         = other.m_status;
            m_acquired       = other.m_acquired;
            other.m_status   = sdk_utils::OK;
            other.m_acquired = false;
        }
        return *this;
    }

    ~SourceLease() { reset(); }

    bool valid() const { return m_acquired; }

    sdk_utils::status_t status() const { return m_status; }

    // 提前归还（探测结束即关句柄）。重复调用安全。
    void reset()
    {
        if (m_acquired && m_source != nullptr) {
            m_source->release();
        }
        m_acquired = false;
        m_status   = sdk_utils::OK;
        m_source.reset();
    }

private:
    SourceLease(const SourceLease &);
    SourceLease &operator=(const SourceLease &);

    std::shared_ptr<DataSource> m_source;
    sdk_utils::status_t m_status     = sdk_utils::OK;
    bool m_acquired                  = false;
};