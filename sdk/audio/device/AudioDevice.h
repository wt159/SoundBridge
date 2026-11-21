#pragma once
#include <IAudioDevice.h>
#include <NonCopyable.hpp>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Audio device class: select, open, close, start, and stop audio devices.
 * @details Supported audio format: 44100 Hz, 16-bit, stereo.
 *
 * Push mode: use write() to push audio data, no callback needed.
 *
 * 实现 IAudioDevice（规格：doc/buffering-switching-spec.md §7.2 / ADR 0011）：
 * 生产路径直接持有本类，不加转发层；测试用装饰器替身包住它。
 *
 * @version 0.2
 * @author wtp (wtp0727@gmail.com)
 * @date 2026-04-02
 * @copyright Copyright (c) 2023
 */
class AudioDevice : public IAudioDevice, public NonCopyable {
public:
    AudioDevice();
    ~AudioDevice() override;

    std::vector<AudDevPair> getDeviceList() override;
    int getDeviceSpec(AudioSpec &spec) override;
    int selectDevice(uint64_t id) override;
    int open() override;
    int close() override;
    bool isOpen() const override;
    int start() override;
    int stop() override;

    // Push mode interfaces
    int write(const void *data, size_t len) override;
    size_t getQueuedBytes() const override;
    void clearQueue() override;

private:
    std::vector<AudDevPair> m_deviceList;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
