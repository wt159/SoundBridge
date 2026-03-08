#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioResample.h"
#include "ErrorUtils.h"
#include "FileSearch.h"
#include "LogWrapper.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct DecodeCollector : public AudioDecodeCallback {
    int frame_count = 0;
    AudioSpec last_spec;

    void onAudioDecodeCallback(AudioDecodeSpec &out) override
    {
        ++frame_count;
        last_spec = out.spec;
    }
};

bool check(bool condition, const char *message)
{
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        return false;
    }
    std::printf("[PASS] %s\n", message);
    return true;
}

bool init_logger()
{
    std::string log_file = "sdk_test_suite";
    std::string log_dir = "./log";
    constexpr int k5MB = 5 * 1024 * 1024;
    constexpr int k5Files = 5;
    LogWrapper::getInstanceInitialize(log_dir, log_file, k5MB, k5Files);
    LOG_INFO("TestSdkSuite", "logger initialized");
    return check(LogWrapper::getInstance() != nullptr, "LogWrapper init");
}

bool test_audio_common()
{
    bool ok = true;
    ok &= check(getAudioFormatByBitPreSample(16) == AudioFormatS16, "Audio format map 16bit");
    ok &= check(getAudioFormatByBitPreSample(32) == AudioFormatS32, "Audio format map 32bit");
    ok &= check(getBytePreSampleByAudioFormat(AudioFormatFLT32) == 4,
                "Audio bytes per sample (float)");
    ok &= check(getBytePreSampleByAudioFormat(AudioFormatDBL64) == 8,
                "Audio bytes per sample (double)");
    return ok;
}

bool test_file_search()
{
    std::vector<std::string> files = recursiveFileSearch(".");
    bool has_wav = false;
    bool has_aac = false;
    for (const auto &path : files) {
        if (path.find("music.wav") != std::string::npos) {
            has_wav = true;
        }
        if (path.find("6-48000_fltp_1.aac") != std::string::npos) {
            has_aac = true;
        }
    }

    bool ok = true;
    ok &= check(!files.empty(), "FileSearch found media files");
    ok &= check(has_wav, "FileSearch found music.wav");
    ok &= check(has_aac, "FileSearch found sample aac");
    return ok;
}

bool test_resample()
{
    std::ifstream in("./1-44100_s16le_2.pcm", std::ios::binary);
    if (!check(in.is_open(), "Open resample input pcm")) {
        return false;
    }

    AudioSpec in_spec;
    in_spec.sampleRate = 44100;
    in_spec.numChannel = 2;
    in_spec.format = AudioFormatS16;
    in_spec.samples = 1024;
    in_spec.bitsPerSample = 16;
    in_spec.bytesPerSample = 2;

    AudioSpec out_spec;
    out_spec.sampleRate = 48000;
    out_spec.numChannel = 1;
    out_spec.format = AudioFormatFLT32;
    out_spec.samples = 1024 * 48000 / 44100;
    out_spec.bitsPerSample = 32;
    out_spec.bytesPerSample = 4;

    AudioResample resample;
    resample.init(in_spec, out_spec);
    if (!check(resample.initCheck() == sdk_utils::OK, "AudioResample initCheck")) {
        return false;
    }

    const size_t in_block =
        static_cast<size_t>(in_spec.samples) * in_spec.numChannel * in_spec.bytesPerSample;
    std::vector<char> in_buf(in_block);
    std::vector<char> out_buf(in_block * 4);

    size_t out_len = 0;
    in.read(in_buf.data(), static_cast<std::streamsize>(in_buf.size()));
    size_t got = static_cast<size_t>(in.gcount());
    if (!check(got > 0, "Read resample input bytes")) {
        return false;
    }

    int ret = resample.resample(in_buf.data(), got, out_buf.data(), &out_len);
    bool ok = true;
    ok &= check(ret == 0, "AudioResample resample return code");
    ok &= check(out_len > 0, "AudioResample output size > 0");
    return ok;
}

bool test_decode()
{
    std::ifstream in("./6-48000_fltp_1.aac", std::ios::binary);
    if (!check(in.is_open(), "Open decode input aac")) {
        return false;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!check(!data.empty(), "Read decode input bytes")) {
        return false;
    }

    DecodeCollector collector;
    AudioDecode decode(AUDIO_CODEC_ID_AAC, &collector);

    bool ok = true;
    ok &= check(decode.initCheck() == sdk_utils::OK, "AudioDecode initCheck");
    if (!ok) {
        return false;
    }

    int ret = decode.decode(data.data(), static_cast<ssize_t>(data.size()));
    ok &= check(ret == 0, "AudioDecode decode return code");
    ok &= check(collector.frame_count > 0, "AudioDecode callback frame count");
    ok &= check(collector.last_spec.sampleRate > 0, "AudioDecode sample rate parsed");
    ok &= check(collector.last_spec.numChannel > 0, "AudioDecode channels parsed");
    ok &= check(collector.last_spec.bytesPerSample > 0, "AudioDecode bytes/sample parsed");
    return ok;
}

bool run_group(const std::string &group)
{
    bool ok = init_logger();

    if (group == "core") {
        ok &= test_audio_common();
        ok &= test_file_search();
        return ok;
    }

    if (group == "resample") {
        ok &= test_resample();
        return ok;
    }

    if (group == "decode") {
        ok &= test_decode();
        return ok;
    }

    if (group == "all") {
        ok &= test_audio_common();
        ok &= test_file_search();
        ok &= test_resample();
        ok &= test_decode();
        return ok;
    }

    std::printf("Unknown group: %s\n", group.c_str());
    std::printf("Valid groups: core | resample | decode | all\n");
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    std::string group = "all";
    if (argc >= 2 && argv[1] != nullptr) {
        group = argv[1];
    }

    bool ok = run_group(group);
    if (!ok) {
        std::printf("\nSDK test group failed: %s\n", group.c_str());
        return 1;
    }

    std::printf("\nSDK test group passed: %s\n", group.c_str());
    return 0;
}
