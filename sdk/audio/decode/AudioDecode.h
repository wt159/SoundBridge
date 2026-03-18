#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ErrorUtils.h"
#include <memory>

struct AudioCodecConfig {
    int sampleRate;
    int channels;
    int bitRate;
    int blockAlign;
    int bitsPerSample;
    AudioBuffer::AudioBufferPtr extraData;

    AudioCodecConfig()
        : sampleRate(0)
        , channels(0)
        , bitRate(0)
        , blockAlign(0)
        , bitsPerSample(0)
        , extraData(nullptr)
    {
    }
};

struct AudioDecodeSpec {
    AudioSpec spec;
    uint8_t **lineData;
    int *lineSize;

    AudioDecodeSpec()
        : lineData(nullptr)
        , lineSize(nullptr)
    {
    }
};

class AudioDecodeCallback {
public:
    virtual ~AudioDecodeCallback() { }
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) = 0;
};

class AudioDecode {
public:
    AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback);
    AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback, const AudioCodecConfig &config);
    ~AudioDecode();
    sdk_utils::status_t initCheck();
    int decode(const char *data, ssize_t size);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
