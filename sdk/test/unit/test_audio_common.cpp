#include <AudioCommon.hpp>
#include <AudioDecode.h>
#include <AudioResample.h>
#include <doctest/doctest.h>
#include <fstream>
#include <vector>

TEST_SUITE("AudioCommon")
{
    TEST_CASE("Audio format mapping 16-bit")
    {
        CHECK(getAudioFormatByBitPreSample(16) == AudioFormatS16);
    }

    TEST_CASE("Audio format mapping 32-bit")
    {
        CHECK(getAudioFormatByBitPreSample(32) == AudioFormatS32);
    }

    TEST_CASE("Audio bytes per sample float")
    {
        CHECK(getBytePreSampleByAudioFormat(AudioFormatFLT32) == 4);
    }

    TEST_CASE("Audio bytes per sample double")
    {
        CHECK(getBytePreSampleByAudioFormat(AudioFormatDBL64) == 8);
    }
}

TEST_SUITE("Resample")
{
    TEST_CASE("AudioResample initCheck")
    {
        AudioSpec inSpec;
        inSpec.sampleRate     = 44100;
        inSpec.numChannel     = 2;
        inSpec.format         = AudioFormatS16;
        inSpec.samples        = 1024;
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;

        AudioSpec outSpec;
        outSpec.sampleRate     = 48000;
        outSpec.numChannel     = 1;
        outSpec.format         = AudioFormatFLT32;
        outSpec.samples        = 1024 * 48000 / 44100;
        outSpec.bitsPerSample  = 32;
        outSpec.bytesPerSample = 4;

        AudioResample resample;
        resample.init(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("AudioResample resample with valid input")
    {
        std::ifstream in("./1-44100_s16le_2.pcm", std::ios::binary);
        if (!in.is_open()) {
            return;
        }

        AudioSpec inSpec;
        inSpec.sampleRate     = 44100;
        inSpec.numChannel     = 2;
        inSpec.format         = AudioFormatS16;
        inSpec.samples        = 1024;
        inSpec.bitsPerSample  = 16;
        inSpec.bytesPerSample = 2;

        AudioSpec outSpec;
        outSpec.sampleRate     = 48000;
        outSpec.numChannel     = 1;
        outSpec.format         = AudioFormatFLT32;
        outSpec.samples        = 1024 * 48000 / 44100;
        outSpec.bitsPerSample  = 32;
        outSpec.bytesPerSample = 4;

        AudioResample resample;
        resample.init(inSpec, outSpec);
        REQUIRE(resample.initCheck() == sdk_utils::OK);

        const size_t inBlock
            = static_cast<size_t>(inSpec.samples) * inSpec.numChannel * inSpec.bytesPerSample;
        std::vector<char> inBuf(inBlock);
        std::vector<char> outBuf(inBlock * 4);

        size_t outLen = outBuf.size();
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        size_t got = static_cast<size_t>(in.gcount());
        REQUIRE(got > 0);

        int ret = resample.resample(inBuf.data(), got, outBuf.data(), &outLen);
        CHECK(ret == 0);
        CHECK(outLen > 0);
    }
}

TEST_SUITE("Decode")
{
    struct TestDecodeCallback : public AudioDecodeCallback {
        int frameCount = 0;
        AudioSpec lastSpec;

        void onAudioDecodeCallback(AudioDecodeSpec &out) override
        {
            ++frameCount;
            lastSpec = out.spec;
        }
    };

    TEST_CASE("AudioDecode initCheck")
    {
        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_AAC, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("AudioDecode decode AAC")
    {
        std::ifstream in("./6-48000_fltp_1.aac", std::ios::binary);
        if (!in.is_open()) {
            return;
        }

        std::vector<char> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        REQUIRE(!data.empty());

        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_AAC, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);

        int ret = decode.decode(data.data(), static_cast<ssize_t>(data.size()));
        CHECK(ret >= 0);
        CHECK(callback.frameCount > 0);
        CHECK(callback.lastSpec.sampleRate > 0);
        CHECK(callback.lastSpec.numChannel > 0);
        CHECK(callback.lastSpec.bytesPerSample > 0);
    }
}
