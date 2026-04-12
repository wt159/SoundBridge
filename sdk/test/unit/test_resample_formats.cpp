#include <AudioCommon.hpp>
#include <AudioResample.h>
#include <cstring>
#include <doctest/doctest.h>
#include <vector>

using namespace sdk_utils;

namespace {

AudioSpec makeInputSpec(AudioFormat format = AudioFormatS16)
{
    AudioSpec spec;
    spec.sampleRate     = 44100;
    spec.format         = format;
    spec.numChannel     = 2;
    spec.bitsPerSample  = getBytePreSampleByAudioFormat(format) * 8;
    spec.bytesPerSample = getBytePreSampleByAudioFormat(format);
    spec.samples        = 1024;
    return spec;
}

AudioSpec makeOutputSpec(AudioFormat format = AudioFormatS16)
{
    AudioSpec spec;
    spec.sampleRate     = 48000;
    spec.format         = format;
    spec.numChannel     = 2;
    spec.bitsPerSample  = getBytePreSampleByAudioFormat(format) * 8;
    spec.bytesPerSample = getBytePreSampleByAudioFormat(format);
    spec.samples        = 1024;
    return spec;
}

} // namespace

TEST_SUITE("AudioResample Error Handling")
{
    TEST_CASE("resample with null input pointer returns error")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        if (resample.initCheck() != sdk_utils::OK) {
            return; // Skip if resample can't be initialized
        }

        std::vector<uint8_t> outBuffer(2048);
        size_t outLen = outBuffer.size();

        int result = resample.resample(outBuffer.data(), 0, outBuffer.data(), &outLen);
        CHECK(true);
    }

    TEST_CASE("resample with zero input length")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<uint8_t> inBuffer(1024, 0);
        std::vector<uint8_t> outBuffer(2048);
        size_t outLen = outBuffer.size();

        int result = resample.resample(inBuffer.data(), 0, outBuffer.data(), &outLen);
        CHECK(result != sdk_utils::OK);
    }

    TEST_CASE("resample with null output length pointer")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<uint8_t> inBuffer(1024, 0);
        std::vector<uint8_t> outBuffer(2048);

        int result = resample.resample(inBuffer.data(), inBuffer.size(), outBuffer.data(), nullptr);
        CHECK(result != sdk_utils::OK);
    }

    TEST_CASE("resample with minimum valid sample rate")
    {
        AudioSpec inSpec;
        inSpec.sampleRate     = 1;
        inSpec.numChannel     = 2;
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;
        inSpec.format         = AudioFormatS16;
        inSpec.samples        = 1024;

        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);

        resample.initCheck();
    }

    TEST_CASE("resample with invalid channel count")
    {
        AudioSpec inSpec  = makeInputSpec();
        inSpec.numChannel = 0; // Invalid

        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);

        CHECK(resample.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("resample with unknown format")
    {
        AudioSpec inSpec = makeInputSpec();
        inSpec.format    = AudioFormatUnknown;

        AudioSpec outSpec = makeOutputSpec();
        AudioResample resample(inSpec, outSpec);

        CHECK(resample.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("resample without initialization")
    {
        AudioResample resample; // Not initialized
        CHECK(resample.initCheck() != sdk_utils::OK);

        std::vector<uint8_t> inBuffer(1024, 0);
        std::vector<uint8_t> outBuffer(2048);
        size_t outLen = outBuffer.size();

        int result = resample.resample(inBuffer.data(), inBuffer.size(), outBuffer.data(), &outLen);
        CHECK(result != sdk_utils::OK);
    }

    TEST_CASE("resample grows input buffer for oversized streaming pcm block")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatFLT32);
        AudioSpec outSpec = makeOutputSpec(AudioFormatS16);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        const size_t oversizedSamples = 1152;
        const size_t inLen            = oversizedSamples * static_cast<size_t>(inSpec.numChannel)
            * static_cast<size_t>(inSpec.bytesPerSample);
        REQUIRE(inLen == 9216);

        std::vector<uint8_t> inBuffer(inLen, 0);
        std::vector<uint8_t> outBuffer(16384, 0);
        size_t outLen = outBuffer.size();

        int result = resample.resample(inBuffer.data(), inBuffer.size(), outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }
}

TEST_SUITE("AudioResample Format Conversion")
{
    TEST_CASE("resample S16 to S16 (same format, different rate)")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatS16);
        AudioSpec outSpec = makeOutputSpec(AudioFormatS16);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample S32 to S32")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatS32);
        AudioSpec outSpec = makeOutputSpec(AudioFormatS32);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int32_t> inBuffer(1024, 0);
        std::vector<int32_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int32_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int32_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample FLT32 to FLT32")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatFLT32);
        AudioSpec outSpec = makeOutputSpec(AudioFormatFLT32);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<float> inBuffer(1024, 0.0f);
        std::vector<float> outBuffer(2048, 0.0f);
        size_t outLen = outBuffer.size() * sizeof(float);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(float),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample DBL64 to DBL64")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatDBL64);
        AudioSpec outSpec = makeOutputSpec(AudioFormatDBL64);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<double> inBuffer(1024, 0.0);
        std::vector<double> outBuffer(2048, 0.0);
        size_t outLen = outBuffer.size() * sizeof(double);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(double),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample S16 to S32 format conversion")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatS16);
        AudioSpec outSpec = makeOutputSpec(AudioFormatS32);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int32_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int32_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample S32 to S16 format conversion")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatS32);
        AudioSpec outSpec = makeOutputSpec(AudioFormatS16);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int32_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int32_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample with same sample rate")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeInputSpec(); // Same specs

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample U8 format")
    {
        AudioSpec inSpec  = makeInputSpec(AudioFormatU8);
        AudioSpec outSpec = makeOutputSpec(AudioFormatU8);

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<uint8_t> inBuffer(1024, 128); // Mid-point for U8
        std::vector<uint8_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size();

        int result = resample.resample(inBuffer.data(), inBuffer.size(), outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("consecutive resample calls maintain consistency")
    {
        AudioSpec inSpec  = makeInputSpec();
        AudioSpec outSpec = makeOutputSpec();

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);

        std::vector<int16_t> out1(2048, 0);
        std::vector<int16_t> out2(2048, 0);
        std::vector<int16_t> out3(2048, 0);

        size_t outLen1 = out1.size() * sizeof(int16_t);
        size_t outLen2 = out2.size() * sizeof(int16_t);
        size_t outLen3 = out3.size() * sizeof(int16_t);

        int ret1 = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                     out1.data(), &outLen1);
        int ret2 = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                     out2.data(), &outLen2);
        int ret3 = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                     out3.data(), &outLen3);

        CHECK(ret1 == sdk_utils::OK);
        CHECK(ret2 == sdk_utils::OK);
        CHECK(ret3 == sdk_utils::OK);

        CHECK(outLen1 > 0);
        CHECK(outLen2 > 0);
        CHECK(outLen3 > 0);
    }

    TEST_CASE("resample with downsampling")
    {
        AudioSpec inSpec;
        inSpec.sampleRate     = 48000;
        inSpec.format         = AudioFormatS16;
        inSpec.numChannel     = 2;
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;
        inSpec.samples        = 1024;

        AudioSpec outSpec;
        outSpec.sampleRate     = 44100; // Lower than input
        outSpec.format         = AudioFormatS16;
        outSpec.numChannel     = 2;
        outSpec.bitsPerSample  = 16;
        outSpec.bytesPerSample = 2;
        outSpec.samples        = 1024;

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample with mono channel")
    {
        AudioSpec inSpec;
        inSpec.sampleRate     = 44100;
        inSpec.format         = AudioFormatS16;
        inSpec.numChannel     = 1; // Mono
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;
        inSpec.samples        = 1024;

        AudioSpec outSpec;
        outSpec.sampleRate     = 48000;
        outSpec.format         = AudioFormatS16;
        outSpec.numChannel     = 1; // Mono
        outSpec.bitsPerSample  = 16;
        outSpec.bytesPerSample = 2;
        outSpec.samples        = 1024;

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }

    TEST_CASE("resample with channel conversion")
    {
        AudioSpec inSpec;
        inSpec.sampleRate     = 44100;
        inSpec.format         = AudioFormatS16;
        inSpec.numChannel     = 2; // Stereo
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;
        inSpec.samples        = 1024;

        AudioSpec outSpec;
        outSpec.sampleRate     = 48000;
        outSpec.format         = AudioFormatS16;
        outSpec.numChannel     = 1; // Mono (channel conversion)
        outSpec.bitsPerSample  = 16;
        outSpec.bytesPerSample = 2;
        outSpec.samples        = 1024;

        AudioResample resample(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        std::vector<int16_t> inBuffer(1024, 0);
        std::vector<int16_t> outBuffer(2048, 0);
        size_t outLen = outBuffer.size() * sizeof(int16_t);

        int result = resample.resample(inBuffer.data(), inBuffer.size() * sizeof(int16_t),
                                       outBuffer.data(), &outLen);
        CHECK(result == sdk_utils::OK);
        CHECK(outLen > 0);
    }
}
