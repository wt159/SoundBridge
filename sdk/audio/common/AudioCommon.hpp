#pragma once

enum AudioFormat
{
    AudioFormatUnknown = -1,
    AudioFormatS8,
    AudioFormatU8,
    AudioFormatS16,
    AudioFormatS16BE,
    AudioFormatU16,
    AudioFormatU16BE,
    AudioFormatS24,
    AudioFormatS24BE,
    AudioFormatU24,
    AudioFormatU24BE,
    AudioFormatS32,
    AudioFormatS32BE,
    AudioFormatFloat32,
    AudioFormatFloat32BE,
    AudioFormatFloat64,
    AudioFormatFloat64BE,
};

int getAudioFormatSize(AudioFormat format)
{
    switch (format)
    {
    case AudioFormatS8:
    case AudioFormatU8:
        return 1;
    case AudioFormatS16:
    case AudioFormatS16BE:
    case AudioFormatU16:
    case AudioFormatU16BE:
        return 2;
    case AudioFormatS24:
    case AudioFormatS24BE:
    case AudioFormatU24:
    case AudioFormatU24BE:
        return 3;
    case AudioFormatS32:
    case AudioFormatS32BE:
    case AudioFormatFloat32:
    case AudioFormatFloat32BE:
        return 4;
    case AudioFormatFloat64:
    case AudioFormatFloat64BE:
        return 8;
    default:
        return 0;
    }
}

struct AudioSpec
{
    int sample_rate;
    AudioFormat format;
    int channels;
    int bits_per_sample;
};
