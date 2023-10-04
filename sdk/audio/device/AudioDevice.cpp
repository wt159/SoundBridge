#include "AudioDevice.h"
#include "LogWrapper.h"
#include <SDL.h>
#include <iostream>
#include <map>

#define LOG_TAG "AudioDevice"
class AudioDevice::Impl {
private:
    using AudioDevSpec = std::pair<std::string, SDL_AudioSpec>;
    AudioDataCallback *m_callback;
    SDL_version m_version;
    int m_deviceNum;
    int m_driverNum;
    SDL_AudioSpec m_sdlSpec;
    bool m_isOpen;
    bool m_isStart;
    std::map<uint64_t, AudioDevSpec> m_devSpecList;
    std::map<uint16_t, std::string> m_driverList;
    static constexpr int m_defaultFreq     = 44100;
    static constexpr int m_defaultChannels = 2;
    static constexpr int m_defaultSamples  = 882;
    static constexpr int m_defaultFormat   = AUDIO_S16SYS;

public:
    Impl(AudioDataCallback *callback);
    ~Impl();
    int getDeviceList(std::vector<AudDevPair> &devList);
    int selectDevice(uint64_t id);
    int getAudioSpec(AudioSpec &spec);
    bool isSupportSpec(AudioSpec &spec);
    AudioSpec getOutputSpec(void);
    int open(AudioSpec &spec);
    void close();
    void start();
    void stop();

private:
    void printInfo();
    void printfSDLAudioSpec(SDL_AudioSpec *spec);
    void SDLAudioSpec2AudioSpec(SDL_AudioSpec *sdlSpec, AudioSpec &spec);
    void AudioSpec2SDLAudioSpec(AudioSpec &spec, SDL_AudioSpec *sdlSpec);
    static void audioCallback(void *userdata, Uint8 *stream, int len);
};

AudioDevice::Impl::Impl(AudioDataCallback *callback)
    : m_callback(callback)
    , m_deviceNum(0)
    , m_driverNum(0)
    , m_isOpen(false)
    , m_isStart(false)
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

    m_sdlSpec.freq     = m_defaultFreq;
    m_sdlSpec.format   = m_defaultFormat;
    m_sdlSpec.channels = m_defaultChannels;
    m_sdlSpec.samples  = m_defaultSamples;
    m_sdlSpec.callback = audioCallback;
    m_sdlSpec.userdata = this;
}

AudioDevice::Impl::~Impl()
{
    SDL_Quit();
}

int AudioDevice::Impl::getDeviceList(std::vector<AudDevPair> &devList)
{
    // TODO: 待实现
    return 0;
}

int AudioDevice::Impl::selectDevice(uint64_t id)
{
    // TODO: 待实现
    return 0;
}

int AudioDevice::Impl::getAudioSpec(AudioSpec &spec)
{
    SDLAudioSpec2AudioSpec(&m_sdlSpec, spec);
    return 0;
}

bool AudioDevice::Impl::isSupportSpec(AudioSpec &spec)
{
    return false;
}

AudioSpec AudioDevice::Impl::getOutputSpec(void)
{
    AudioSpec spec;
    getAudioSpec(spec);
    return spec;
}

int AudioDevice::Impl::open(AudioSpec &spec)
{
    if (m_isOpen) {
        LOG_ERROR(LOG_TAG, "Audio device is already open");
        return -1;
    }
    LOG_DEBUG(LOG_TAG, "sdl spec format: %d", m_sdlSpec.format);
    // SDL_AudioSpec supportSpec;
    int ret = SDL_OpenAudio(&m_sdlSpec, nullptr);
    if (ret != 0) {
        LOG_ERROR(LOG_TAG, "SDL_OpenAudio failed: %s", SDL_GetError());
        return -1;
    }
    printfSDLAudioSpec(&m_sdlSpec);
    m_isOpen = true;
    return 0;
}

void AudioDevice::Impl::close()
{
    if (!m_isOpen) {
        LOG_ERROR(LOG_TAG, "Audio device is not open");
        return;
    }
    SDL_CloseAudio();
    m_isOpen = false;
}

void AudioDevice::Impl::start()
{
    if (m_isStart || !m_isOpen) {
        LOG_ERROR(LOG_TAG, "Audio device is already start or not open");
        return;
    }
    SDL_PauseAudio(0);
    m_isStart = true;
}

void AudioDevice::Impl::stop()
{
    if (!m_isStart || !m_isOpen) {
        LOG_ERROR(LOG_TAG, "Audio device is not start or not open");
        return;
    }
    SDL_PauseAudio(1);
    m_isStart = false;
}

void AudioDevice::Impl::printInfo()
{
    LOG_INFO(LOG_TAG, "SDL version: %d.%d.%d", (int)m_version.major, (int)m_version.minor,
             (int)m_version.patch);
    LOG_INFO(LOG_TAG, "Audio device number: %d", m_deviceNum);
    for (auto &dev : m_devSpecList) {
        LOG_INFO(LOG_TAG, "----------------------------------------");
        LOG_INFO(LOG_TAG, "Device ID: %d", dev.first);
        LOG_INFO(LOG_TAG, "Device name: %s", dev.second.first.data());
        LOG_INFO(LOG_TAG, "Device spec: ");
        LOG_INFO(LOG_TAG, "  freq: %d", dev.second.second.freq);
        LOG_INFO(LOG_TAG, "  format: %d", dev.second.second.format);
        LOG_INFO(LOG_TAG, "  channels: %d", (int)dev.second.second.channels);
        LOG_INFO(LOG_TAG, "  silence: %d", (int)dev.second.second.silence);
        LOG_INFO(LOG_TAG, "  samples: %d", dev.second.second.samples);
        LOG_INFO(LOG_TAG, "  size: %d", dev.second.second.size);
        LOG_INFO(LOG_TAG, "  callback: %p", (void *)dev.second.second.callback);
        LOG_INFO(LOG_TAG, "  userdata: %p", (void *)dev.second.second.userdata);
        LOG_INFO(LOG_TAG, "----------------------------------------");
    }
    LOG_INFO(LOG_TAG, "----------------------------------------");
    LOG_INFO(LOG_TAG, "Audio Driver number: %d", m_driverNum);
    for (auto &driver : m_driverList) {
        LOG_INFO(LOG_TAG, "----------------------------------------");
        LOG_INFO(LOG_TAG, "Driver ID: %d", driver.first);
        LOG_INFO(LOG_TAG, "Driver name: %s", driver.second.data());
        LOG_INFO(LOG_TAG, "----------------------------------------");
    }
}

void AudioDevice::Impl::printfSDLAudioSpec(SDL_AudioSpec *spec)
{
    LOG_DEBUG(LOG_TAG, "----------------------------------------");
    LOG_DEBUG(LOG_TAG, "  freq: %d", spec->freq);
    LOG_DEBUG(LOG_TAG, "  format: %d", spec->format);
    LOG_DEBUG(LOG_TAG, "  channels: %d", (int)spec->channels);
    LOG_DEBUG(LOG_TAG, "  silence: %d", (int)spec->silence);
    LOG_DEBUG(LOG_TAG, "  samples: %d", spec->samples);
    LOG_DEBUG(LOG_TAG, "  size: %d", spec->size);
    LOG_DEBUG(LOG_TAG, "  callback: %p", (void *)spec->callback);
    LOG_DEBUG(LOG_TAG, "  userdata: %p", (void *)spec->userdata);
    LOG_DEBUG(LOG_TAG, "----------------------------------------");
}

void AudioDevice::Impl::SDLAudioSpec2AudioSpec(SDL_AudioSpec *sdlSpec, AudioSpec &spec)
{
    spec.sampleRate = sdlSpec->freq;
    spec.numChannel = sdlSpec->channels;
    switch (sdlSpec->format) {
    case AUDIO_U8:
        spec.format = AudioFormatU8;
        break;
    case AUDIO_S8:
        spec.format = AudioFormatS8;
        break;
    case AUDIO_U16:
        spec.format = AudioFormatU16;
        break;
    case AUDIO_U16MSB:
        spec.format = AudioFormatU16BE;
        break;
    case AUDIO_S16:
        spec.format = AudioFormatS16;
        break;
    case AUDIO_S16MSB:
        spec.format = AudioFormatS16BE;
        break;
    case AUDIO_S32:
        spec.format = AudioFormatS32;
        break;
    case AUDIO_S32MSB:
        spec.format = AudioFormatS32BE;
        break;
    case AUDIO_F32:
        spec.format = AudioFormatFLT32;
        break;
    case AUDIO_F32MSB:
        spec.format = AudioFormatFLT32BE;
        break;
    default:
        LOG_FATAL(LOG_TAG, "Audio format not supported");
        break;
    }
    spec.bytesPerSample = getAudioFormatSize(spec.format);
    spec.bitsPerSample  = spec.bytesPerSample * 8;
}

void AudioDevice::Impl::AudioSpec2SDLAudioSpec(AudioSpec &spec, SDL_AudioSpec *sdlSpec)
{
    sdlSpec->freq     = spec.sampleRate;
    sdlSpec->channels = spec.numChannel;
    switch (spec.format) {
    case AudioFormatU8:
        sdlSpec->format = AUDIO_U8;
        break;
    case AudioFormatS8:
        sdlSpec->format = AUDIO_S8;
        break;
    case AudioFormatU16:
        sdlSpec->format = AUDIO_U16;
        break;
    case AudioFormatU16BE:
        sdlSpec->format = AUDIO_U16MSB;
        break;
    case AudioFormatS16:
        sdlSpec->format = AUDIO_S16;
        break;
    case AudioFormatS16BE:
        sdlSpec->format = AUDIO_S16MSB;
        break;
    case AudioFormatS32:
        sdlSpec->format = AUDIO_S32;
        break;
    case AudioFormatS32BE:
        sdlSpec->format = AUDIO_S32MSB;
        break;
    case AudioFormatFLT32:
        sdlSpec->format = AUDIO_F32;
        break;
    case AudioFormatFLT32BE:
        sdlSpec->format = AUDIO_F32MSB;
        break;
    default:
        LOG_FATAL(LOG_TAG, "Audio format not supported");
        break;
    }
}

void AudioDevice::Impl::audioCallback(void *userdata, Uint8 *stream, int len)
{
    AudioDevice::Impl *impl = (AudioDevice::Impl *)userdata;
    impl->m_callback->getAudioData(stream, len);
}

AudioDevice::AudioDevice(AudioDataCallback *callback)
    : m_impl(new Impl(callback))
{
}

AudioDevice::~AudioDevice() { }

std::vector<AudioDevice::AudDevPair> AudioDevice::getDeviceList()
{
    std::vector<AudDevPair> devList;
    m_impl->getDeviceList(devList);
    return devList;
}

int AudioDevice::getDeviceSpec(AudioSpec &spec)
{
    return m_impl->getAudioSpec(spec);
}

int AudioDevice::selectDevice(uint64_t id)
{
    return m_impl->selectDevice(id);
}

bool AudioDevice::isSupportSpec(AudioSpec &spec)
{
    return m_impl->isSupportSpec(spec);
}

AudioSpec AudioDevice::getOutputSpec()
{
    return m_impl->getOutputSpec();
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
