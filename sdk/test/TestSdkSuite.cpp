#include "../include/soundbridge/player.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "AudioDecodeProcess.h"
#include "AudioResample.h"
#include "ErrorUtils.h"
#include "ExtractorFactory.h"
#include "FileSearch.h"
#include "FileSource.h"
#include "LogWrapper.h"
#include "MusicPlayer.h"
#include "fixtures/RealMediaFixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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

struct LegacyPlayerProbe : public sdk::MusicPlayerListener {
    std::atomic<int> error_count { 0 };
    std::atomic<int> state_change_count { 0 };
    std::mutex mutex;
    std::string last_trace_id;

    void onMusicPlayerStateChanged(sdk::MusicPlayerState) override
    {
        state_change_count.fetch_add(1);
    }
    void onMusicPlayerListCurrentIndexChanged(int) override { }
    void onMusicPlayerDurationChanged(uint64_t) override { }
    void onMusicPlayerPositionChanged(uint64_t) override { }
    void onMusicPlayerMusicListChanged(std::list<sdk::MusicIndex>) override { }
    void onMusicPlayerError(sdk::ErrorCode, const std::string &, int, const std::string &,
                            const std::string &traceId) override
    {
        error_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        last_trace_id = traceId;
    }
};

struct PublicPlayerProbe : public soundbridge::PlayerCallbacks {
    std::atomic<int> error_count { 0 };
    std::atomic<int> track_changed_count { 0 };
    std::atomic<int> last_track_index { -1 };
    std::mutex mutex;
    std::string last_trace_id;
    std::map<int, std::string> track_names;

    void onStateChanged(soundbridge::PlayerState) override { }
    void onTrackChanged(int index) override
    {
        track_changed_count.fetch_add(1);
        last_track_index.store(index);
    }
    void onDurationChanged(uint64_t) override { }
    void onPositionChanged(uint64_t) override { }
    void onPlaylistChanged(const std::list<soundbridge::TrackInfo> &tracks) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        track_names.clear();
        for (const auto &track : tracks) {
            track_names[track.index] = track.name;
        }
    }
    void onError(soundbridge::ErrorCode, const std::string &, int, const std::string &,
                 const std::string &traceId) override
    {
        error_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(mutex);
        last_trace_id = traceId;
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
    std::string log_file  = "sdk_test_suite";
    std::string log_dir   = "./log";
    constexpr int k5MB    = 5 * 1024 * 1024;
    constexpr int k5Files = 5;
    LogWrapper::getInstanceInitialize(log_dir, log_file, k5MB, k5Files);
    LOG_INFO("TestSdkSuite", "logger initialized");
    return check(LogWrapper::getInstance() != nullptr, "LogWrapper init");
}

template <typename Predicate> bool wait_for_condition(Predicate predicate, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

bool test_audio_common()
{
    bool ok  = true;
    ok      &= check(getAudioFormatByBitPreSample(16) == AudioFormatS16, "Audio format map 16bit");
    ok      &= check(getAudioFormatByBitPreSample(32) == AudioFormatS32, "Audio format map 32bit");
    ok      &= check(getBytePreSampleByAudioFormat(AudioFormatFLT32) == 4,
                     "Audio bytes per sample (float)");
    ok      &= check(getBytePreSampleByAudioFormat(AudioFormatDBL64) == 8,
                     "Audio bytes per sample (double)");
    return ok;
}

bool test_file_search()
{
    std::vector<std::string> files = recursiveFileSearch(".");
    bool has_wav                   = false;
    bool has_aac                   = false;
    for (const auto &path : files) {
        if (path.find("music.wav") != std::string::npos) {
            has_wav = true;
        }
        if (path.find("6-48000_fltp_1.aac") != std::string::npos) {
            has_aac = true;
        }
    }

    bool ok  = true;
    ok      &= check(!files.empty(), "FileSearch found media files");
    ok      &= check(has_wav, "FileSearch found music.wav");
    ok      &= check(has_aac, "FileSearch found sample aac");
    return ok;
}

bool test_resample()
{
    std::ifstream in("./1-44100_s16le_2.pcm", std::ios::binary);
    if (!check(in.is_open(), "Open resample input pcm")) {
        return false;
    }

    AudioSpec in_spec;
    in_spec.sampleRate     = 44100;
    in_spec.numChannel     = 2;
    in_spec.format         = AudioFormatS16;
    in_spec.samples        = 1024;
    in_spec.bitsPerSample  = 16;
    in_spec.bytesPerSample = 2;

    AudioSpec out_spec;
    out_spec.sampleRate     = 48000;
    out_spec.numChannel     = 1;
    out_spec.format         = AudioFormatFLT32;
    out_spec.samples        = 1024 * 48000 / 44100;
    out_spec.bitsPerSample  = 32;
    out_spec.bytesPerSample = 4;

    AudioResample resample;
    resample.init(in_spec, out_spec);
    if (!check(resample.initCheck() == sdk_utils::OK, "AudioResample initCheck")) {
        return false;
    }

    const size_t in_block
        = static_cast<size_t>(in_spec.samples) * in_spec.numChannel * in_spec.bytesPerSample;
    std::vector<char> in_buf(in_block);
    std::vector<char> out_buf(in_block * 4);

    size_t out_len = out_buf.size(); // 传递输出缓冲区大小
    in.read(in_buf.data(), static_cast<std::streamsize>(in_buf.size()));
    size_t got = static_cast<size_t>(in.gcount());
    if (!check(got > 0, "Read resample input bytes")) {
        return false;
    }

    int ret  = resample.resample(in_buf.data(), got, out_buf.data(), &out_len);
    bool ok  = true;
    ok      &= check(ret == 0, "AudioResample resample return code");
    ok      &= check(out_len > 0, "AudioResample output size > 0");
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

    bool ok  = true;
    ok      &= check(decode.initCheck() == sdk_utils::OK, "AudioDecode initCheck");
    if (!ok) {
        return false;
    }

    int ret  = decode.decode(data.data(), static_cast<ssize_t>(data.size()));
    ok      &= check(ret == 0, "AudioDecode decode return code");
    ok      &= check(collector.frame_count > 0, "AudioDecode callback frame count");
    ok      &= check(collector.last_spec.sampleRate > 0, "AudioDecode sample rate parsed");
    ok      &= check(collector.last_spec.numChannel > 0, "AudioDecode channels parsed");
    ok      &= check(collector.last_spec.bytesPerSample > 0, "AudioDecode bytes/sample parsed");
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
    int limit             = get_env_int("SB_MEDIA_LIMIT", 0);
    std::string filter    = normalize_ext_filter(get_env_or_default("SB_MEDIA_FILTER", ""));
    std::vector<std::string> files = recursiveFileSearch(media_dir);
    if (!check(!files.empty(), "MediaSmoke found media files")) {
        return false;
    }
    std::sort(files.begin(), files.end());

    bool ok   = true;
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
            fails.push_back({ path, "open" });
            ok = false;
            continue;
        }

        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(source.get(), ext, true));
        if (!check(extractor != nullptr, ("MediaSmoke extractor " + path).c_str())) {
            fails.push_back({ path, "extractor" });
            ok = false;
            continue;
        }
        if (!check(extractor->initCheck() == sdk_utils::OK,
                   ("MediaSmoke extractor init " + path).c_str())) {
            fails.push_back({ path, "extractor_init" });
            ok = false;
            continue;
        }

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        if (!check(decode && decode->initCheck() == sdk_utils::OK,
                   ("MediaSmoke decode init " + path).c_str())) {
            AudioSpec spec                    = extractor->getAudioSpec();
            AudioBuffer::AudioBufferPtr extra = extractor->getCodecExtraData();
            std::printf("DecodeInit info: codec=%#x, sr=%d, ch=%d, bits=%d, bitrate=%d, "
                        "blockAlign=%d, extra=%zu\n",
                        extractor->getAudioCodecID(), spec.sampleRate, spec.numChannel,
                        spec.bitsPerSample, extractor->getBitRate(), extractor->getBlockAlign(),
                        extra ? extra->size() : 0);
            fails.push_back({ path, "decode_init" });
            ok = false;
            continue;
        }

        AudioBuffer::AudioBufferPtr out = decode->getDecodeBuffer();
        if (!check(out != nullptr && out->size() > 0,
                   ("MediaSmoke decoded bytes " + path).c_str())) {
            fails.push_back({ path, "decoded_bytes" });
            ok = false;
            continue;
        }
    }

    std::printf("MediaSmoke decoded files: %d (filter=%s)\n", count,
                filter.empty() ? "<none>" : filter.c_str());
    if (!fails.empty()) {
        std::printf("MediaSmoke failures: %zu\n", fails.size());
        for (const auto &f : fails) {
            std::printf("FAIL: %s | stage=%s\n", f.path.c_str(), f.stage.c_str());
        }
    }
    return ok;
}

bool test_player_api()
{
    bool ok = true;

    {
        std::string log_dir = "./log";
        LegacyPlayerProbe probe;
        sdk::MusicPlayer player(static_cast<sdk::MusicPlayerListener *>(&probe), log_dir);
        ok &= check(player.playbackMode() == sdk::MusicPlaybackMode::Sequential,
                    "MusicPlayer default playback mode is sequential");
        player.setPlaybackMode(sdk::MusicPlaybackMode::CurrentItemOnce);
        ok &= check(player.playbackMode() == sdk::MusicPlaybackMode::CurrentItemOnce,
                    "MusicPlayer stores single once playback mode");
        player.setPlaybackMode(sdk::MusicPlaybackMode::CurrentItemInLoop);
        ok &= check(player.playbackMode() == sdk::MusicPlaybackMode::CurrentItemInLoop,
                    "MusicPlayer stores single loop playback mode");
        player.setPlaybackMode(sdk::MusicPlaybackMode::Random);
        ok &= check(player.playbackMode() == sdk::MusicPlaybackMode::Random,
                    "MusicPlayer stores random playback mode");
        player.play();
        ok &= check(wait_for_condition([&probe]() { return probe.error_count.load() > 0; }, 3000),
                    "MusicPlayer reports error when playing without current track");
        std::string traceId;
        {
            std::lock_guard<std::mutex> lock(probe.mutex);
            traceId = probe.last_trace_id;
        }
        ok &= check(!traceId.empty(), "MusicPlayer error callback includes trace id");
    }

    {
        PublicPlayerProbe probe;
        soundbridge::PlayerConfig config;
        config.logDirectory = "./log";
        soundbridge::Player player(static_cast<soundbridge::PlayerCallbacks *>(&probe), config);
        ok &= check(player.playbackMode() == soundbridge::PlaybackMode::Sequential,
                    "Public Player default playback mode is sequential");
        player.setPlaybackMode(soundbridge::PlaybackMode::SingleOnce);
        ok &= check(player.playbackMode() == soundbridge::PlaybackMode::SingleOnce,
                    "Public Player stores single once playback mode");
        player.setPlaybackMode(soundbridge::PlaybackMode::SingleLoop);
        ok &= check(player.playbackMode() == soundbridge::PlaybackMode::SingleLoop,
                    "Public Player stores single loop playback mode");
        player.setPlaybackMode(soundbridge::PlaybackMode::Random);
        ok &= check(player.playbackMode() == soundbridge::PlaybackMode::Random,
                    "Public Player stores random playback mode");
        player.play();
        ok &= check(wait_for_condition([&probe]() { return probe.error_count.load() > 0; }, 3000),
                    "Public Player forwards error callback without current track");
        std::string traceId;
        {
            std::lock_guard<std::mutex> lock(probe.mutex);
            traceId = probe.last_trace_id;
        }
        ok &= check(!traceId.empty(), "Public Player error callback includes trace id");
        player.addMusicDirectory("../../music");
        ok &= check(wait_for_condition([&player]() { return player.trackCount() > 0; }, 3000),
                    "Public Player loads tracks through facade");
        ok &= check(
            wait_for_condition([&player]() { return !player.currentTrackName().empty(); }, 3000),
            "Public Player exposes current track name from core");
        ok &= check(wait_for_condition([&player]() { return player.duration() > 0; }, 3000),
                    "Public Player exposes current duration from core");
        ok &= check(player.position() == 0, "Public Player initial position stays at zero");
        if (player.trackCount() > 1) {
            player.setCurrentTrack(1);
            ok &= check(
                wait_for_condition([&probe]() { return probe.last_track_index.load() == 1; }, 3000),
                "Public Player reports track switch through public callbacks");
            ok &= check(wait_for_condition(
                            [&player, &probe]() {
                                std::lock_guard<std::mutex> lock(probe.mutex);
                                auto iter = probe.track_names.find(1);
                                return iter != probe.track_names.end() && !iter->second.empty()
                                    && player.currentTrackName() == iter->second;
                            },
                            3000),
                        "Public Player current track name matches callback playlist after switch");
            ok &= check(wait_for_condition([&player]() { return player.duration() > 0; }, 3000),
                        "Public Player keeps duration available after track switch");
            ok &= check(player.position() == 0, "Public Player resets position after track switch");
        }
    }

    return ok;
}

bool test_extractor_single(const char *ext, const char *filename)
{
    printf("  test_extractor_single: START\n");
    fflush(stdout);

    RealMediaFixture fixture;
    std::string path = fixture.mediaPath(filename);
    printf("  test_extractor_single: path=%s\n", path.c_str());
    fflush(stdout);

    printf("  test_extractor_single: creating FileSource\n");
    fflush(stdout);
    std::shared_ptr<FileSource> src;
    std::unique_ptr<ExtractorHelper> extHelper = fixture.create(filename, ext, src);
    printf("  test_extractor_single: FileSource created, initCheck=%d\n",
           src ? src->initCheck() : sdk_utils::NAME_NOT_FOUND);
    fflush(stdout);

    if (!src || src->initCheck() != sdk_utils::OK) {
        printf("[SKIP] %s (file not found)\n", ext);
        return true;
    }
    printf("  test_extractor_single: extractor created\n");
    fflush(stdout);

    bool ok = extHelper && extHelper->initCheck() == sdk_utils::OK;
    printf("  %s initCheck: %s\n", ext, ok ? "OK" : "FAIL");
    fflush(stdout);

    if (ok) {
        AudioSpec s = extHelper->getAudioSpec();
        printf("  sampleRate: %d, numChannel: %d, durationMs: %zu\n", s.sampleRate, s.numChannel,
               s.durationMs);
    }

    printf("  about to destroy extractor...\n");
    fflush(stdout);
    extHelper.reset();
    printf("  extractor destroyed\n");
    fflush(stdout);

    printf("  about to destroy FileSource...\n");
    fflush(stdout);
    src.reset();
    printf("  FileSource destroyed\n");
    fflush(stdout);

    printf("  %s completed\n", ext);
    fflush(stdout);
    return ok;
}

bool test_extractor_isolated()
{
    printf("\n=== Testing extractors one by one ===\n");
    fflush(stdout);

    printf("\n1. Testing WAV...\n");
    fflush(stdout);
    bool ok = test_extractor_single(".wav", "music.wav");
    printf("   WAV done, ok=%d\n", ok);

    printf("\n2. Testing AIFF...\n");
    ok &= test_extractor_single(".aiff", "俱乐部音乐循环-Freesound.aiff");
    printf("   AIFF done, ok=%d\n", ok);

    printf("\n3. Testing FLAC...\n");
    ok &= test_extractor_single(".flac", "小镇姑娘-陶喆.flac");
    printf("   FLAC done, ok=%d\n", ok);

    printf("\n4. Testing MP3...\n");
    ok &= test_extractor_single(".mp3", "消愁-毛不易.320.mp3");
    printf("   MP3 done, ok=%d\n", ok);

    printf("\n5. Testing OGG...\n");
    ok &= test_extractor_single(".ogg", "Ringtones-耳聆网.ogg");
    printf("   OGG done, ok=%d\n", ok);

    printf("\n6. Testing AAC...\n");
    ok &= test_extractor_single(".aac", "48000_fltp_1.aac");
    printf("   AAC done, ok=%d\n", ok);

    printf("\n7. Testing M4A...\n");
    ok &= test_extractor_single(".m4a", "摇滚乐_Freesound.m4a");
    printf("   M4A done, ok=%d\n", ok);

    printf("\n8. Testing ASF...\n");
    ok &= test_extractor_single(".asf", "萨克斯机.asf");
    printf("   ASF done, ok=%d\n", ok);

    printf("\n9. Testing APE...\n");
    ok &= test_extractor_single(".ape", "music-ape.ape");
    printf("   APE done, ok=%d\n", ok);

    printf("\n10. Testing MKV...\n");
    ok &= test_extractor_single(".mkv", "music-mkv.mkv");
    printf("   MKV done, ok=%d\n", ok);

    printf("\n11. Testing WMA...\n");
    ok &= test_extractor_single(".wma", "十七岁-陶喆.96.wma");
    printf("   WMA done, ok=%d\n", ok);

    printf("\n12. Testing AMR...\n");
    ok &= test_extractor_single(".amr", "小镇姑娘-陶喆.96.amr");
    printf("   AMR done, ok=%d\n", ok);

    printf("\n=== All isolated tests done ===\n\n");
    return ok;
}

bool test_extractor_spec()
{
    bool ok = true;
    RealMediaFixture fixture;

    // WAV
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("music.wav", ".wav", "ExtractorSpec wav", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec wav initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec wav sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec wav numChannel");
                ok          &= check(s.bitsPerSample > 0, "ExtractorSpec wav bitsPerSample");
                ok          &= check(s.format != AudioFormatUnknown, "ExtractorSpec wav format");
                ok          &= check(s.durationMs > 0, "ExtractorSpec wav durationMs");
            }
        }
    }

    // AIFF
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("\xe4\xbf\xb1\xe4\xb9\x90\xe9\x83\xa8\xe9\x9f\xb3\xe4\xb9"
                               "\x90\xe5\xbe\xaa\xe7\x8e\xaf-Freesound.aiff",
                               ".aiff", "ExtractorSpec aiff", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec aiff initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec aiff sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec aiff numChannel");
                ok          &= check(s.bitsPerSample > 0, "ExtractorSpec aiff bitsPerSample");
                ok          &= check(s.format != AudioFormatUnknown, "ExtractorSpec aiff format");
                ok          &= check(s.durationMs > 0, "ExtractorSpec aiff durationMs");
            }
        }
    }

    // FLAC
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip(
                "\xe5\xb0\x8f\xe9\x95\x87\xe5\xa7\x91\xe5\xa8\x98-\xe9\x99\xb6\xe5\x96\x86.flac",
                ".flac", "ExtractorSpec flac", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec flac initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec flac sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec flac numChannel");
                ok          &= check(s.bitsPerSample > 0, "ExtractorSpec flac bitsPerSample");
                ok          &= check(s.format != AudioFormatUnknown, "ExtractorSpec flac format");
                ok          &= check(s.durationMs > 0, "ExtractorSpec flac durationMs");
            }
        }
    }

    // MP3
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip(
                "\xe6\xb6\x88\xe6\x84\x81-\xe6\xaf\x9b\xe4\xb8\x8d\xe6\x98\x93.320.mp3", ".mp3",
                "ExtractorSpec mp3", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec mp3 initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec spec  = ext->getAudioSpec();
                ok             &= check(spec.sampleRate > 0, "ExtractorSpec mp3 sampleRate");
                ok             &= check(spec.numChannel > 0, "ExtractorSpec mp3 numChannel");
                ok             &= check(spec.durationMs > 0, "ExtractorSpec mp3 durationMs");
                // MP3 is lossy: bitsPerSample/format not asserted
            }
        }
    }

    // OGG
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("Ringtones-\xe8\x80\xb3\xe8\x81\x86\xe7\xbd\x91.ogg", ".ogg",
                               "ExtractorSpec ogg", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec ogg initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec ogg sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec ogg numChannel");
                ok          &= check(s.durationMs >= 0, "ExtractorSpec ogg durationMs");
                // OGG/Vorbis: bitsPerSample/format NOT asserted (lossy)
            }
        }
    }

    // AAC
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("48000_fltp_1.aac", ".aac", "ExtractorSpec aac", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec aac initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec aac sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec aac numChannel");
                ok          &= check(s.bitsPerSample == 16, "ExtractorSpec aac bitsPerSample");
                ok          &= check(s.format != AudioFormatUnknown, "ExtractorSpec aac format");
                ok          &= check(s.durationMs > 0, "ExtractorSpec aac durationMs");
            }
        }
    }

    // M4A
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("\xe6\x91\x87\xe6\xbb\x9a\xe4\xb9\x90_Freesound.m4a", ".m4a",
                               "ExtractorSpec m4a", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec m4a initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec m4a sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec m4a numChannel");
                ok          &= check(s.durationMs > 0, "ExtractorSpec m4a durationMs");
            }
        }
    }

    // ASF
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("\xe8\x90\xa8\xe5\x85\x8b\xe6\x96\xaf\xe6\x9c\xba.asf", ".asf",
                               "ExtractorSpec asf", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec asf initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec asf sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec asf numChannel");
                ok          &= check(s.durationMs > 0, "ExtractorSpec asf durationMs");
            }
        }
    }

    // APE
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("music-ape.ape", ".ape", "ExtractorSpec ape", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec ape initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec ape sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec ape numChannel");
                ok          &= check(s.bitsPerSample > 0, "ExtractorSpec ape bitsPerSample");
                ok          &= check(s.format != AudioFormatUnknown, "ExtractorSpec ape format");
                ok          &= check(s.durationMs > 0, "ExtractorSpec ape durationMs");
            }
        }
    }

    // MKV
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip("music-mkv.mkv", ".mkv", "ExtractorSpec mkv", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec mkv initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec mkv sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec mkv numChannel");
                ok          &= check(s.durationMs > 0, "ExtractorSpec mkv durationMs");
            }
        }
    }

    // WMA (uses ASFExtractor)
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip(
                "\xe5\x8d\x81\xe4\xb8\x83\xe5\xb2\xb3-\xe9\x99\xb6\xe5\x96\x86.96.wma", ".wma",
                "ExtractorSpec wma", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec wma initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec wma sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec wma numChannel");
                ok          &= check(s.durationMs > 0, "ExtractorSpec wma durationMs");
            }
        }
    }

    // AMR (uses ASFExtractor)
    {
        std::shared_ptr<FileSource> src;
        std::unique_ptr<ExtractorHelper> ext;
        if (fixture.openOrSkip(
                "\xe5\xb0\x8f\xe9\x95\x87\xe5\xa7\x91\xe5\xa8\x98-\xe9\x99\xb6\xe5\x96\x86.96.amr",
                ".amr", "ExtractorSpec amr", src, ext)) {
            ok &= check(ext && ext->initCheck() == sdk_utils::OK, "ExtractorSpec amr initCheck");
            if (ext && ext->initCheck() == sdk_utils::OK) {
                AudioSpec s  = ext->getAudioSpec();
                ok          &= check(s.sampleRate > 0, "ExtractorSpec amr sampleRate");
                ok          &= check(s.numChannel > 0, "ExtractorSpec amr numChannel");
                ok          &= check(s.durationMs > 0, "ExtractorSpec amr durationMs");
            }
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

    if (group == "extractor") {
        ok &= test_extractor_spec();
        return ok;
    }

    if (group == "extractor_iso") {
        ok &= test_extractor_isolated();
        return ok;
    }

    if (group == "player") {
        ok &= test_player_api();
        return ok;
    }

    if (group == "all") {
        ok &= test_audio_common();
        ok &= test_file_search();
        ok &= test_resample();
        ok &= test_decode();
        ok &= test_extractor_spec();
        ok &= test_player_api();
        return ok;
    }

    std::printf("Unknown group: %s\n", group.c_str());
    std::printf("Valid groups: core | resample | decode | media | extractor | player | all\n");
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
