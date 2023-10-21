#pragma once
#include "AudioCommon.hpp"
#include "ErrorUtils.h"
#include <memory>

struct AudioDecodeSpec
{
    AudioSpec spec;
    uint8_t **lineData;
    int *lineSize;
};

class AudioDecodeCallback
{
public:
    virtual ~AudioDecodeCallback() {}
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) = 0;
};

class AudioDecode
{
public:
    AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback);
    ~AudioDecode();
    sdk_utils::status_t initCheck();
    int decode(const char *data, ssize_t size);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};