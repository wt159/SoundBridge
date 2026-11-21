#pragma once
#include <AudioCommon.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief 音频输出设备的抽象接口。
 * @details 生产路径的唯一实现是 AudioDevice；测试侧用装饰器替身包住它，按脚本注入失败。
 *          决策见 doc/buffering-switching-spec.md §7.2 与 docs/adr/0011。
 *
 * 接口覆盖 AudioDevice 的全部公开实例方法。设备枚举与选择也在接口内，但生产侧只是
 * 转发 SDL；替身不承担设备表保真责任。静态测试钩子不进本接口。
 */
class IAudioDevice {
public:
    using AudDevPair = std::pair<uint64_t, std::string>;

    virtual ~IAudioDevice() = default;

    virtual std::vector<AudDevPair> getDeviceList() = 0;
    virtual int getDeviceSpec(AudioSpec &spec) = 0;
    virtual int selectDevice(uint64_t id) = 0;
    virtual int open() = 0;
    virtual int close() = 0;
    virtual bool isOpen() const = 0;
    // 返回真实结果：未 open / 重复 start 视为失败（-1），成功为 0。
    virtual int start() = 0;
    virtual int stop() = 0;
    // Push mode：成功返回 0，失败返回负数（沿用 SDL_QueueAudio 的返回值语义）。
    virtual int write(const void *data, size_t len) = 0;
    virtual size_t getQueuedBytes() const = 0;
    virtual void clearQueue() = 0;
};
