#pragma once
#include "IAudioDevice.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

/**
 * @brief 设备装饰器替身（规格 §7.2 / ADR 0011）。
 * @details 包住内层 IAudioDevice（通常就是真实 AudioDevice），默认全直通；按脚本让指定
 *          次数的调用返回 -1，其余调用原样转发，因此 dummy 队列的真实节拍不受影响。
 *
 * 脚本形状是一次性计数：第 N 次命中目标的调用失败一次，其余放行。静音分支的判据用
 * 「仅全零负载的 write()」这一路，分支归属由脚本条件保证，不靠旁证推断。
 * `close()` 清零所有脚本，与 deviceWriteFailures 按播放任务累计的口径对齐。
 */
class AudioDeviceDouble : public IAudioDevice {
public:
    explicit AudioDeviceDouble(std::shared_ptr<IAudioDevice> inner)
        : m_inner(std::move(inner))
    {
    }

    // ---- 脚本（一次性计数，0 表示未装） ----
    // open 失败既有一次性脚本，也有开关：后者用于「误跳轨的第二次 play 也必须失败」。
    void setOpenFailureEnabled(bool enabled) { m_openFailSwitch.store(enabled); }
    void failOpenAtCall(size_t ordinal) { m_openFailAt.store(ordinal); }
    void failStartAtCall(size_t ordinal) { m_startFailAt.store(ordinal); }
    void failWriteAtCall(size_t ordinal) { m_writeFailAt.store(ordinal); }
    void failZeroWriteAtCall(size_t ordinal) { m_zeroWriteFailAt.store(ordinal); }

    // ---- 计数（只增，供判据取证） ----
    size_t openCalls() const { return m_openCalls.load(); }
    size_t startCalls() const { return m_startCalls.load(); }
    size_t writeCalls() const { return m_writeCalls.load(); }
    size_t zeroWriteCalls() const { return m_zeroWriteCalls.load(); }

    // IAudioDevice
    std::vector<AudDevPair> getDeviceList() override { return m_inner->getDeviceList(); }

    int getDeviceSpec(AudioSpec &spec) override { return m_inner->getDeviceSpec(spec); }

    int selectDevice(uint64_t id) override { return m_inner->selectDevice(id); }

    int open() override
    {
        const size_t ordinal = m_openCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (consumeOnce(m_openFailAt, ordinal)) {
            return -1;
        }
        if (m_openFailSwitch.load(std::memory_order_acquire)) {
            return -1;
        }
        return m_inner->open();
    }

    int close() override
    {
        resetScript();
        return m_inner->close();
    }

    bool isOpen() const override { return m_inner->isOpen(); }

    int start() override
    {
        const size_t ordinal = m_startCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (consumeOnce(m_startFailAt, ordinal)) {
            return -1;
        }
        return m_inner->start();
    }

    int stop() override { return m_inner->stop(); }

    int write(const void *data, size_t len) override
    {
        const size_t ordinal = m_writeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        size_t zeroOrdinal     = 0;
        if (isAllZero(data, len)) {
            zeroOrdinal = m_zeroWriteCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (consumeOnce(m_writeFailAt, ordinal)
            || (zeroOrdinal != 0 && consumeOnce(m_zeroWriteFailAt, zeroOrdinal))) {
            return -1;
        }
        return m_inner->write(data, len);
    }

    size_t getQueuedBytes() const override { return m_inner->getQueuedBytes(); }

    void clearQueue() override { m_inner->clearQueue(); }

private:
    static bool isAllZero(const void *data, size_t len)
    {
        const unsigned char *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < len; ++i) {
            if (bytes[i] != 0) {
                return false;
            }
        }
        return true;
    }

    static bool consumeOnce(std::atomic<size_t> &armed, size_t ordinal)
    {
        if (armed.load(std::memory_order_acquire) != ordinal) {
            return false;
        }
        armed.store(0, std::memory_order_release);
        return true;
    }

    void resetScript()
    {
        m_openFailSwitch.store(false);
        m_openFailAt.store(0);
        m_startFailAt.store(0);
        m_writeFailAt.store(0);
        m_zeroWriteFailAt.store(0);
    }

    std::shared_ptr<IAudioDevice> m_inner;
    std::atomic<bool> m_openFailSwitch { false };
    std::atomic<size_t> m_openFailAt { 0 };
    std::atomic<size_t> m_startFailAt { 0 };
    std::atomic<size_t> m_writeFailAt { 0 };
    std::atomic<size_t> m_zeroWriteFailAt { 0 };
    std::atomic<size_t> m_openCalls { 0 };
    std::atomic<size_t> m_startCalls { 0 };
    std::atomic<size_t> m_writeCalls { 0 };
    std::atomic<size_t> m_zeroWriteCalls { 0 };
};
