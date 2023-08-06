#include "AudioDevice.h"
#include <SDL.h>
#include <iostream>
#include <map>
class AudioDevice::Impl {
private:
    using AudioDevSpec = std::pair<std::string, SDL_AudioSpec>;
    SDL_version m_version;
    int m_deviceNum;
    int m_driverNum;
    SDL_AudioSpec m_spec;
    bool m_isOpen;
    bool m_isStart;
    std::map<uint64_t, AudioDevSpec> m_devSpecList;
    std::map<uint16_t, std::string> m_driverList;
    AudioDataCallback *m_callback;

public:
    Impl(AudioDataCallback *callback);
    ~Impl();
    int open(AudioSpec &spec);
    void close();
    void start();
    void stop();

private:
    void printInfo();
    static void audioCallback(void *userdata, Uint8 *stream, int len);
};

AudioDevice::Impl::Impl(AudioDataCallback *callback)
    : m_callback(callback)
{
    SDL_VERSION(&m_version);
    m_devSpecList.clear();
    m_deviceNum = SDL_GetNumAudioDevices(0);
    SDL_AudioSpec spec;
    for (int i = 0; i < m_deviceNum; i++) {
        std::string devName = SDL_GetAudioDeviceName(i, 0);
        SDL_GetAudioDeviceSpec(i, 0, &spec);
        m_devSpecList.emplace(std::make_pair(i, std::make_pair(devName, spec)));
    }
    m_driverList.clear();
    m_driverNum = SDL_GetNumAudioDrivers();
    for (int i = 0; i < m_driverNum; i++) {
        m_driverList.emplace(std::make_pair(i, SDL_GetAudioDriver(i)));
    }

    printInfo();

    SDL_Init(SDL_INIT_AUDIO);

    m_spec.samples = 512;
    m_spec.callback = audioCallback;
    m_spec.userdata = this;
}

AudioDevice::Impl::~Impl()
{
    SDL_Quit();
}

int AudioDevice::Impl::open(AudioSpec &spec)
{
    m_spec.freq = spec.sample_rate;
    m_spec.channels = spec.channels;
    switch (spec.format) {
    case AudioFormatU8:
        m_spec.format = AUDIO_U8;
        break;
    case AudioFormatS8:
        m_spec.format = AUDIO_S8;
        break;
    case AudioFormatU16:
        m_spec.format = AUDIO_U16;
        break;
    case AudioFormatU16BE:
        m_spec.format = AUDIO_U16MSB;
        break;
    case AudioFormatS16:
        m_spec.format = AUDIO_S16;
        break;
    case AudioFormatS16BE:
        m_spec.format = AUDIO_S16MSB;
        break;
    case AudioFormatS32:
        m_spec.format = AUDIO_S32;
        break;
    case AudioFormatS32BE:
        m_spec.format = AUDIO_S32MSB;
        break;
    case AudioFormatFLT32:
        m_spec.format = AUDIO_F32;
        break;
    case AudioFormatFLT32BE:
        m_spec.format = AUDIO_F32MSB;
        break;
    default:
        std::cout << "Audio format not supported" << std::endl;
        break;
    }
    int ret = SDL_OpenAudio(&m_spec, nullptr);
    if (ret != 0) {
        std::cout << "SDL_OpenAudio failed: " << SDL_GetError() << std::endl;
        return -1;
    }
    m_isOpen = true;
    return 0;
}

void AudioDevice::Impl::close()
{
    if (!m_isOpen)
        return;
    SDL_CloseAudio();
    m_isOpen = false;
}

void AudioDevice::Impl::start()
{
    if (m_isStart || !m_isOpen)
        return;
    SDL_PauseAudio(0);
    m_isStart = true;
}

void AudioDevice::Impl::stop()
{
    if (!m_isStart || !m_isOpen)
        return;
    SDL_PauseAudio(1);
    m_isStart = false;
}

void AudioDevice::Impl::printInfo()
{
    std::cout << "SDL version: " << (int)m_version.major << "." << (int)m_version.minor << "."
              << (int)m_version.patch << std::endl;
    std::cout << "Audio device number: " << m_deviceNum << std::endl;
    for (auto &dev : m_devSpecList) {
        std::cout << "Device ID: " << dev.first << std::endl;
        std::cout << "Device name: " << dev.second.first << std::endl;
        std::cout << "Device spec: " << std::endl;
        std::cout << "  freq: " << dev.second.second.freq << std::endl;
        std::cout << "  format: " << dev.second.second.format << std::endl;
        std::cout << "  channels: " << (int)dev.second.second.channels << std::endl;
        std::cout << "  silence: " << (int)dev.second.second.silence << std::endl;
        std::cout << "  samples: " << dev.second.second.samples << std::endl;
        std::cout << "  size: " << dev.second.second.size << std::endl;
        std::cout << "  callback: " << (void *)dev.second.second.callback << std::endl;
        std::cout << "  userdata: " << (void *)dev.second.second.userdata << std::endl;
    }
    std::cout << "Audio driver number: " << m_driverNum << std::endl;
    std::cout << "   id  name" << std::endl;
    for (auto &driver : m_driverList) {
        std::cout << "  [" << driver.first << "] ";
        std::cout << " [" << driver.second << "]" << std::endl;
    }
}

void AudioDevice::Impl::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioDevice::Impl *impl = (AudioDevice::Impl *)userdata;
    impl->m_callback->onAudioData(stream, len);
}

AudioDevice::AudioDevice(AudioDataCallback *callback)
    : m_impl(new Impl(callback))
{
}

AudioDevice::~AudioDevice() { }

std::vector<AudioDevice::AudDevPair> AudioDevice::getDeviceList()
{
    return std::vector<AudDevPair>();
}

int AudioDevice::open(AudioSpec &spec)
{
    return m_impl->open(spec);
}

int AudioDevice::close()
{
    m_impl->close();
    return 0;
}

int AudioDevice::start()
{
    m_impl->start();
    return 0;
}

int AudioDevice::stop()
{
    m_impl->stop();
    return 0;
}
