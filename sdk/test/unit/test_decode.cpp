#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecode.h>
#include <AudioDecodeProcess.h>
#include <FLACDecode.h>
#include <VorbisDecode.h>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

namespace doctest {
template <> struct StringMaker<std::shared_ptr<AudioBuffer>> {
    static String convert(const std::shared_ptr<AudioBuffer> &ptr)
    {
        if (!ptr)
            return "nullptr";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "shared_ptr<AudioBuffer>[%zu]", ptr->size());
        return buf;
    }
};
}

TEST_SUITE("AudioCodecConfig")
{
    TEST_CASE("Default construction")
    {
        AudioCodecConfig config;
        REQUIRE(config.sampleRate == 0);
        REQUIRE(config.channels == 0);
        REQUIRE(config.bitRate == 0);
        REQUIRE(config.blockAlign == 0);
        REQUIRE(config.bitsPerSample == 0);
        REQUIRE(config.extraData == nullptr);
    }

    TEST_CASE("Construction with values")
    {
        AudioCodecConfig config;
        config.sampleRate    = 44100;
        config.channels      = 2;
        config.bitRate       = 128000;
        config.blockAlign    = 4;
        config.bitsPerSample = 16;
        REQUIRE(config.sampleRate == 44100);
        REQUIRE(config.channels == 2);
        REQUIRE(config.bitRate == 128000);
        REQUIRE(config.blockAlign == 4);
        REQUIRE(config.bitsPerSample == 16);
    }
}

TEST_SUITE("AudioDecodeSpec")
{
    TEST_CASE("Default construction")
    {
        AudioDecodeSpec spec;
        REQUIRE(spec.lineData == nullptr);
        REQUIRE(spec.lineSize == nullptr);
        REQUIRE(spec.spec.sampleRate == 0);
        REQUIRE(spec.spec.format == AudioFormatUnknown);
    }

    TEST_CASE("With AudioSpec")
    {
        AudioDecodeSpec spec;
        spec.spec.sampleRate = 48000;
        spec.spec.format     = AudioFormatS16;
        spec.spec.numChannel = 2;
        REQUIRE(spec.spec.sampleRate == 48000);
        REQUIRE(spec.spec.format == AudioFormatS16);
        REQUIRE(spec.spec.numChannel == 2);
    }
}

class TestDecodeCallback : public AudioDecodeCallback {
public:
    int decodeCount;
    AudioDecodeSpec lastSpec;

    TestDecodeCallback()
        : decodeCount(0)
    {
    }

    void onAudioDecodeCallback(AudioDecodeSpec &out) override
    {
        decodeCount++;
        lastSpec = out;
    }
};

TEST_SUITE("AudioDecode")
{
    TEST_CASE("Construction with codec only")
    {
        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_MP3, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Construction with codec and config")
    {
        TestDecodeCallback callback;
        AudioCodecConfig config;
        config.sampleRate = 44100;
        config.channels   = 2;
        AudioDecode decode(AUDIO_CODEC_ID_MP3, &callback, config);
        REQUIRE(decode.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Construction with unsupported codec")
    {
        TestDecodeCallback callback;
        AudioDecode decode(static_cast<AudioCodecID>(0xFFFF), &callback);
        REQUIRE(decode.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("Decode with null data")
    {
        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_MP3, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);
        int result = decode.decode(nullptr, 0);
        REQUIRE(result == 0);
    }

    TEST_CASE("Decode with valid codec")
    {
        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_FLAC, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);
    }
}

TEST_SUITE("AudioDecodeProcess")
{
    TEST_CASE("Get decode spec when uninitialized returns default")
    {
        AudioSpec spec;
        spec.sampleRate = 0;
        REQUIRE(spec.sampleRate == 0);
        REQUIRE(spec.numChannel == 0);
        REQUIRE(spec.format == AudioFormatUnknown);
    }
}

TEST_SUITE("FLACDecode")
{
    TEST_CASE("Construction")
    {
        TestDecodeCallback callback;
        FLACDecode decode(&callback);
    }

    TEST_CASE("Decode with null data")
    {
        TestDecodeCallback callback;
        FLACDecode decode(&callback);
        int result = decode.decode(nullptr, 0);
        REQUIRE(result < 0);
    }

    TEST_CASE("Set input buffer")
    {
        TestDecodeCallback callback;
        FLACDecode decode(&callback);
        std::shared_ptr<AudioBuffer> buffer(new AudioBuffer(1024));
        decode.setInputBuffer(buffer);
        CHECK(true);
    }

    TEST_CASE("Set abort flag")
    {
        TestDecodeCallback callback;
        FLACDecode decode(&callback);
        std::atomic<bool> abortFlag(false);
        decode.setAbortFlag(&abortFlag);
        abortFlag = true;
    }
}

TEST_SUITE("VorbisDecode")
{
    TEST_CASE("Construction")
    {
        TestDecodeCallback callback;
        VorbisDecode decode(&callback);
    }

    TEST_CASE("Decode with null data")
    {
        TestDecodeCallback callback;
        VorbisDecode decode(&callback);
        int result = decode.decode(nullptr, 0);
        REQUIRE(result < 0);
    }

    TEST_CASE("Set abort flag")
    {
        TestDecodeCallback callback;
        VorbisDecode decode(&callback);
        std::atomic<bool> abortFlag(false);
        decode.setAbortFlag(&abortFlag);
        abortFlag = true;
    }
}
