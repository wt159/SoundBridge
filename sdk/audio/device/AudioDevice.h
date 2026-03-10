#pragma once
#include <NonCopyable.hpp>
#include <AudioCommon.hpp>
#include <memory>
#include <string>
#include <vector>

class AudioDataCallback {
public:
    virtual ~AudioDataCallback() = default;
    virtual void getAudioData(void *data, int len) = 0;
};

/**
 * @brief Audio device class: select, open, close, start, and stop audio devices.
 * @details Supported audio format: 44100 Hz, 16-bit, stereo.
  * 
 * @version 0.1
 * @author wtp (wtp0727@gmail.com)
 * @date 2023-10-03
 * @copyright Copyright (c) 2023
 */
class AudioDevice : public NonCopyable {
public:
    using AudDevPair = std::pair<uint64_t, std::string>;

public:
    AudioDevice() = delete;
    AudioDevice(AudioDataCallback *callback);
    ~AudioDevice();
    std::vector<AudDevPair> getDeviceList();
    int getDeviceSpec(AudioSpec &spec);
    int selectDevice(uint64_t id);
    int open();
    int close();
    int start();
    int stop();

private:
    std::vector<AudDevPair> m_deviceList;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
