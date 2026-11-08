#pragma once
#include <AudioCommon.hpp>
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
 * @version 0.2
 * @author wtp (wtp0727@gmail.com)
 * @date 2026-04-02
 * @copyright Copyright (c) 2023
 */
class AudioDevice : public NonCopyable {
public:
    using AudDevPair = std::pair<uint64_t, std::string>;

public:
    AudioDevice();
    ~AudioDevice();
    std::vector<AudDevPair> getDeviceList();
    int getDeviceSpec(AudioSpec &spec);
    int selectDevice(uint64_t id);
    int open();
    // 仅供单元测试（票 #110）：强制 open() 返回失败，用来构造「设备错误 →
    // error→state」的端到端判据。默认关闭；读点只在控制线程调用 open() 时，
    // 不进 Impl::open()、不碰 SDL。AudioDevice 是 SDK 内部类型，本钩子不进入
    // soundbridge::Player 的对外公开 API（票 #109 的一次限定例外）。
    static void setForceOpenFailureForTest(bool enabled);
    int close();
    bool isOpen() const;
    int start();
    int stop();

    // Push mode interfaces
    int write(const void *data, size_t len);
    size_t getQueuedBytes() const;
    void clearQueue();

private:
    std::vector<AudDevPair> m_deviceList;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
