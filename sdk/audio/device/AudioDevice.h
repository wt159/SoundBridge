#pragma once
#include <NonCopyable.hpp>
#include <AudioCommon.hpp>
#include <memory>
#include <string>
#include <vector>

class AudioDataCallback {
public:
    virtual ~AudioDataCallback() = default;
    virtual void onAudioData(void *data, int len) = 0;
};
class AudioDevice : public NonCopyable {
public:
    using AudDevPair = std::pair<uint64_t, std::string>;

public:
    AudioDevice() = delete;
    AudioDevice(AudioDataCallback *callback);
    ~AudioDevice();
    std::vector<AudDevPair> getDeviceList();
    int selectDevice(uint64_t id);
    int open(AudioSpec &spec);
    int close();
    int start();
    int stop();

private:
    std::vector<AudDevPair> m_deviceList;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};