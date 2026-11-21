#include <AudioCommon.hpp>
#include <AudioDecode.h>
#include <AudioResample.h>
#include <PcmGain.hpp>
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

TEST_SUITE("PcmGain")
{
    // 软件增益（ADR 0013）的纯函数部分：不需要声卡，也不碰线程。
    TEST_CASE("S16 half gain halves every sample")
    {
        int16_t samples[4] = {1000, -1000, 32767, -32768};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(samples), sizeof(samples), AudioFormatS16,
                              0.5f, 0.5f));
        CHECK(samples[0] == 500);
        CHECK(samples[1] == -500);
        CHECK(samples[2] == 16384);
        CHECK(samples[3] == -16384);
    }

    TEST_CASE("zero gain produces silence")
    {
        int16_t samples[8];
        for (int16_t &sample : samples) {
            sample = 12345;
        }
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(samples), sizeof(samples), AudioFormatS16,
                              0.0f, 0.0f));
        for (const int16_t sample : samples) {
            CHECK(sample == 0);
        }
    }

    TEST_CASE("unsigned 8-bit scales around its midpoint")
    {
        uint8_t samples[3] = {128, 255, 0};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(samples), sizeof(samples), AudioFormatU8,
                              1.0f, 1.0f));
        CHECK(samples[0] == 128);
        CHECK(samples[1] == 255);
        CHECK(samples[2] == 0);

        // 零增益必须落到零点 128，而不是 0——否则静音会带一个直流偏置。
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(samples), sizeof(samples), AudioFormatU8,
                              0.0f, 0.0f));
        for (const uint8_t sample : samples) {
            CHECK(sample == 128);
        }
    }

    TEST_CASE("S32 and FLT32 scale by their own full scale")
    {
        // 支持的格式不止 S16（ADR 0013 的格式分派表）：S32 / FLT32 必须各按自身位宽缩放，
        // 被误当成 S16 解释的后果是样本数算成两倍、字节读串。
        int32_t wide[2] = {1000000, -1000000};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(wide), sizeof(wide), AudioFormatS32,
                                   0.5f, 0.5f));
        CHECK(wide[0] == 500000);
        CHECK(wide[1] == -500000);

        float floats[2] = {0.8f, -0.8f};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(floats), sizeof(floats),
                                   AudioFormatFLT32, 0.5f, 0.5f));
        CHECK(floats[0] == doctest::Approx(0.4f));
        CHECK(floats[1] == doctest::Approx(-0.4f));

        // 浮点按 [-1, 1] 钳制：0.9 × 2.0 不许溢出到 >1。
        float loud[1] = {0.9f};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(loud), sizeof(loud), AudioFormatFLT32,
                                   2.0f, 2.0f));
        CHECK(loud[0] == doctest::Approx(1.0f));
    }

    TEST_CASE("ramp moves monotonically to the target gain")
    {
        int16_t samples[5] = {1000, 1000, 1000, 1000, 1000};
        REQUIRE(sdk::applyGainRamp(reinterpret_cast<char *>(samples), sizeof(samples), AudioFormatS16,
                              1.0f, 0.0f));
        CHECK(samples[0] == 1000);
        CHECK(samples[4] == 0);
        CHECK(samples[0] > samples[2]);
        CHECK(samples[2] > samples[4]);
    }

    TEST_CASE("unsupported format is left untouched")
    {
        char bytes[6] = {1, 2, 3, 4, 5, 6};
        CHECK_FALSE(sdk::applyGainRamp(bytes, sizeof(bytes), AudioFormatS24, 0.0f, 0.0f));
        for (int i = 0; i < 6; ++i) {
            CHECK(bytes[i] == static_cast<char>(i + 1));
        }
    }
}
