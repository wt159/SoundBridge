#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioDecodeProcess.h"
#include "AudioResample.h"
#include "ErrorUtils.h"
#include "ExtractorFactory.h"
#include "FileSearch.h"
#include "FileSource.h"
#include "LogWrapper.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <algorithm>
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

std::string get_env_or_default(const char *name, const std::string &fallback)
{
    const char *val = std::getenv(name);
    if (val && val[0] != '\0') {
        return std::string(val);
    }
    return fallback;
}

int get_env_int(const char *name, int fallback)
{
    const char *val = std::getenv(name);
    if (!val || val[0] == '\0') {
        return fallback;
    }
    return std::atoi(val);
}

std::string lower_ext(std::string ext)
{
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') {
            return static_cast<char>(c - 'A' + 'a');
        }
        return static_cast<char>(c);
    });
    return ext;
}

std::string normalize_ext_filter(const std::string &raw)
{
    if (raw.empty()) {
        return std::string();
    }
    std::string out = lower_ext(raw);
    if (out[0] != '.') {
        out = "." + out;
    }
    return out;
}

bool test_media_smoke()
{
    std::string media_dir = get_env_or_default("SB_MEDIA_DIR", "../../music");
    int limit = get_env_int("SB_MEDIA_LIMIT", 0);
    std::string filter = normalize_ext_filter(get_env_or_default("SB_MEDIA_FILTER", ""));
    std::vector<std::string> files = recursiveFileSearch(media_dir);
    if (!check(!files.empty(), "MediaSmoke found media files")) {
        return false;
    }
    std::sort(files.begin(), files.end());

    bool ok = true;
    int count = 0;
    struct FailItem {
        std::string path;
        std::string stage;
    };
    std::vector<FailItem> fails;
    for (const auto &path : files) {
        std::string ext;
        size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            ext = path.substr(dot);
        }
        ext = lower_ext(ext);
        if (!filter.empty() && ext != filter) {
            continue;
        }
        if (limit > 0 && count >= limit) {
            break;
        }
        count++;

        std::shared_ptr<FileSource> source(new FileSource(path.c_str()));
        if (!check(source && source->initCheck() == sdk_utils::OK,
                   ("MediaSmoke open file " + path).c_str())) {
            fails.push_back({path, "open"});
            ok = false;
            continue;
        }

        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(source.get(), ext, true));
        if (!check(extractor != nullptr, ("MediaSmoke extractor " + path).c_str())) {
            fails.push_back({path, "extractor"});
            ok = false;
            continue;
        }
        if (!check(extractor->initCheck() == sdk_utils::OK,
                   ("MediaSmoke extractor init " + path).c_str())) {
            fails.push_back({path, "extractor_init"});
            ok = false;
            continue;
        }

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        if (!check(decode && decode->initCheck() == sdk_utils::OK,
                   ("MediaSmoke decode init " + path).c_str())) {
            AudioSpec spec = extractor->getAudioSpec();
            AudioBuffer::AudioBufferPtr extra = extractor->getCodecExtraData();
            std::printf("DecodeInit info: codec=%#x, sr=%d, ch=%d, bits=%d, bitrate=%d, blockAlign=%d, extra=%zu\n",
                        extractor->getAudioCodecID(),
                        spec.sampleRate,
                        spec.numChannel,
                        spec.bitsPerSample,
                        extractor->getBitRate(),
                        extractor->getBlockAlign(),
                        extra ? extra->size() : 0);
            fails.push_back({path, "decode_init"});
            ok = false;
            continue;
        }

        AudioBuffer::AudioBufferPtr out = decode->getDecodeBuffer();
        if (!check(out != nullptr && out->size() > 0,
                   ("MediaSmoke decoded bytes " + path).c_str())) {
            fails.push_back({path, "decoded_bytes"});
            ok = false;
            continue;
        }
    }

    std::printf("MediaSmoke decoded files: %d (filter=%s)\n",
                count, filter.empty() ? "<none>" : filter.c_str());
    if (!fails.empty()) {
        std::printf("MediaSmoke failures: %zu\n", fails.size());
        for (const auto &f : fails) {
            std::printf("FAIL: %s | stage=%s\n", f.path.c_str(), f.stage.c_str());
        }
    }
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

    if (group == "media") {
        ok &= test_media_smoke();
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
    std::printf("Valid groups: core | resample | decode | media | all\n");
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    std::string group = "all";
    if (argc >= 2 && argv[1] != nullptr) {
        group = argv[1];
    }
    if (group == "media") {
        if (argc >= 3 && argv[2] != nullptr) {
#if defined(_WIN32)
            _putenv_s("SB_MEDIA_FILTER", argv[2]);
#else
            setenv("SB_MEDIA_FILTER", argv[2], 1);
#endif
        }
        if (argc >= 4 && argv[3] != nullptr) {
#if defined(_WIN32)
            _putenv_s("SB_MEDIA_LIMIT", argv[3]);
#else
            setenv("SB_MEDIA_LIMIT", argv[3], 1);
#endif
        }
    }

    bool ok = run_group(group);
    if (!ok) {
        std::printf("\nSDK test group failed: %s\n", group.c_str());
        return 1;
    }

    std::printf("\nSDK test group passed: %s\n", group.c_str());
    return 0;
}
