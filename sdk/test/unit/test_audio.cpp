#include <AudioCommon.hpp>
#include <AudioResample.h>
#include <AudioRingBuffer.h>
#include <cmath>
#include <cstring>
#include <doctest/doctest.h>
#include <iostream>
#include <vector>

TEST_SUITE("AudioResample")
{
    AudioSpec makeInputSpec()
    {
        AudioSpec spec;
        spec.sampleRate    = 44100;
        spec.format        = AudioFormatS16;
        spec.numChannel    = 2;
        spec.bitsPerSample = 16;
        spec.samples       = 1024;
        return spec;
    }

    AudioSpec makeOutputSpec()
    {
        AudioSpec spec;
        spec.sampleRate    = 48000;
        spec.format        = AudioFormatS16;
        spec.numChannel    = 2;
        spec.bitsPerSample = 16;
        spec.samples       = 1024;
        return spec;
    }

    TEST_CASE("Construction with specs")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Init after construction with specs")
    {
        AudioResample resample;
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();
        resample.init(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Resample with zero input length")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        int16_t dummy = 0;
        size_t outLen = 1024;

        int result = resample.resample(&dummy, 0, &dummy, &outLen);
        REQUIRE(result != sdk_utils::OK);
    }

    TEST_CASE("Resample with invalid output length")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        int16_t dummy = 0;
        size_t outLen = 0;

        int result = resample.resample(&dummy, 1024, &dummy, &outLen);
        REQUIRE(result != sdk_utils::OK);
    }

    TEST_CASE("Resample with valid input")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        REQUIRE(result == sdk_utils::OK);
        REQUIRE(outLen > 0);
    }

    TEST_CASE("Resample with null output pointer")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        const size_t kBufferSize = 4096;
        std::vector<int16_t> inBuffer(kBufferSize / sizeof(int16_t), 0);
        size_t outLen = 0;

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t), nullptr,
                                       &outLen);
        REQUIRE(result != sdk_utils::OK);
    }
}

TEST_SUITE("AudioFormat utilities")
{
    TEST_CASE("getAudioFormatByBitPreSample")
    {
        REQUIRE(getAudioFormatByBitPreSample(8) == AudioFormatU8);
        REQUIRE(getAudioFormatByBitPreSample(16) == AudioFormatS16);
        REQUIRE(getAudioFormatByBitPreSample(24) == AudioFormatS24);
        REQUIRE(getAudioFormatByBitPreSample(32) == AudioFormatS32);
        REQUIRE(getAudioFormatByBitPreSample(64) == AudioFormatDBL64);
        REQUIRE(getAudioFormatByBitPreSample(0) == AudioFormatUnknown);
        REQUIRE(getAudioFormatByBitPreSample(7) == AudioFormatUnknown);
    }

    TEST_CASE("getBytePreSampleByAudioFormat")
    {
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatU8) == 1);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatS16) == 2);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatS24) == 3);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatS32) == 4);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatFLT32) == 4);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatDBL64) == 8);
        REQUIRE(getBytePreSampleByAudioFormat(AudioFormatUnknown) == 0);
    }
}

TEST_SUITE("AudioSpec")
{
    TEST_CASE("Default construction")
    {
        AudioSpec spec;
        REQUIRE(spec.sampleRate == 0);
        REQUIRE(spec.format == AudioFormatUnknown);
        REQUIRE(spec.numChannel == 0);
        REQUIRE(spec.bitsPerSample == 0);
        REQUIRE(spec.bytesPerSample == 0);
        REQUIRE(spec.samples == 0);
        REQUIRE(spec.durationMs == 0);
    }

    TEST_CASE("Operator equality")
    {
        AudioSpec spec1;
        spec1.sampleRate = 44100;
        spec1.format     = AudioFormatS16;
        spec1.numChannel = 2;

        AudioSpec spec2;
        spec2.sampleRate = 44100;
        spec2.format     = AudioFormatS16;
        spec2.numChannel = 2;

        AudioSpec spec3;
        spec3.sampleRate = 48000;
        spec3.format     = AudioFormatS16;
        spec3.numChannel = 2;

        REQUIRE(spec1 == spec2);
        REQUIRE_FALSE(spec1 == spec3);
    }
}
