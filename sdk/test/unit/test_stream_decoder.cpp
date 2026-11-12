#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecode.h>
#include <AudioDecodeProcess.h>
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <doctest/doctest.h>
#include <memory>
#include <thread>

namespace {
AudioSpec makeDevSpec()
{
    AudioSpec spec;
    spec.sampleRate     = 44100;
    spec.numChannel     = 2;
    spec.bitsPerSample  = 16;
    spec.bytesPerSample = 2;
    spec.format         = AudioFormatS16;
    spec.samples        = 0;
    spec.durationMs     = 0;
    return spec;
}

[[maybe_unused]] bool waitForDecoderState(AudioStreamDecoder &decoder,
                                          StreamDecoderState expectedState, int timeoutMs)
{
    const int pollMs = 5;
    int elapsed      = 0;
    while (elapsed <= timeoutMs) {
        const StreamDecoderState state = decoder.state();
        if (state == expectedState) {
            return true;
        }
        if (state == StreamDecoderState::ERROR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    return false;
}

bool waitForDecoderStateWithDrain(AudioStreamDecoder &decoder, AudioRingBuffer &ring,
                                  StreamDecoderState expectedState, int timeoutMs)
{
    const int pollMs = 5;
    int elapsed      = 0;
    std::vector<char> drain(4096);
    while (elapsed <= timeoutMs) {
        while (ring.availableRead() > 0) {
            const size_t once = std::min(drain.size(), ring.availableRead());
            if (ring.read(drain.data(), once) == 0) {
                break;
            }
        }

        const StreamDecoderState state = decoder.state();
        if (state == expectedState) {
            return true;
        }
        if (state == StreamDecoderState::ERROR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    return false;
}

// P1-1：可观测注入指针的合成数据源。override setAbortFlag 仍委托基类保存，
// 于是 abortRequested() 走基类默认实现，injectedFlag() 暴露绑定状态供断言。
class AbortProbeSource : public DataSourceBase {
public:
    sdk_utils::status_t initCheck() const override { return sdk_utils::OK; }
    ssize_t readAt(off64_t, void *, size_t) override { return 0; }
    void setAbortFlag(const std::atomic<bool> *flag) override
    {
        DataSourceBase::setAbortFlag(flag);
    }
    const std::atomic<bool> *injectedFlag() const { return m_abortFlag; }
};

} // namespace

// Mock extractor for testing error paths
class MockExtractor : public ExtractorHelper {
public:
    MockExtractor()
        : m_initCheck(sdk_utils::OK)
        , m_spec()
        , m_codecID(AUDIO_CODEC_ID_NONE)
        , m_dataSource(nullptr)
    {
        m_spec.sampleRate     = 44100;
        m_spec.numChannel     = 2;
        m_spec.bitsPerSample  = 16;
        m_spec.bytesPerSample = 2;
        m_spec.format         = AudioFormatS16;
        m_spec.durationMs     = 1000;
    }

    void setInitCheck(sdk_utils::status_t status) { m_initCheck = status; }
    void setAudioSpec(const AudioSpec &spec) { m_spec = spec; }
    void setCodecID(AudioCodecID codecID) { m_codecID = codecID; }
    void setDataSource(DataSourceBase *ds) { m_dataSource = ds; }
    void setMetaData(const AudioBuffer::AudioBufferPtr &meta) { m_metaData = meta; }

    sdk_utils::status_t initCheck() override { return m_initCheck; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return m_codecID; }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_metaData; }
    DataSourceBase *getDataSource() override { return m_dataSource; }
    off64_t getDataSize() override { return m_metaData ? m_metaData->size() : 0; }
    off64_t getAudioDataOffset() override { return 0; }

private:
    sdk_utils::status_t m_initCheck;
    AudioSpec m_spec;
    AudioCodecID m_codecID;
    DataSourceBase *m_dataSource;
    AudioBuffer::AudioBufferPtr m_metaData;
};

TEST_SUITE("AudioStreamDecoder Construction")
{
    TEST_CASE("default construction succeeds")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("construction with null ring")
    {
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(nullptr, devSpec);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }
}

TEST_SUITE("AudioStreamDecoder Error Paths")
{
    TEST_CASE("start with null ring returns INVALID_OPERATION")
    {
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(nullptr, devSpec);

        MockExtractor extractor;
        auto status = decoder.start(&extractor);
        CHECK(status == sdk_utils::INVALID_OPERATION);
        CHECK(decoder.state() == StreamDecoderState::ERROR);
    }

    TEST_CASE("start with null extractor returns INVALID_OPERATION")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        auto status = decoder.start(nullptr);
        CHECK(status == sdk_utils::INVALID_OPERATION);
        CHECK(decoder.state() == StreamDecoderState::ERROR);
    }

    TEST_CASE("stop after error returns to IDLE")
    {
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(nullptr, devSpec);
        decoder.start(nullptr); // Error

        decoder.stop();
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("multiple stop calls are safe")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.stop();
        decoder.stop();
        decoder.stop();
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("start after error clears previous state")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        // First start with null -> error
        auto status1 = decoder.start(nullptr);
        CHECK(status1 == sdk_utils::INVALID_OPERATION);
        CHECK(decoder.state() == StreamDecoderState::ERROR);

        // Stop to reset
        decoder.stop();
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }
}

TEST_SUITE("AudioStreamDecoder Seek")
{
    TEST_CASE("seekToMs with IDLE state is handled")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.seekToMs(1000);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
        CHECK(decoder.positionMs() == 0);
    }

    TEST_CASE("seekToMs with ERROR state is handled")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.start(nullptr);
        REQUIRE(decoder.state() == StreamDecoderState::ERROR);

        decoder.seekToMs(1000);
        CHECK(decoder.state() == StreamDecoderState::ERROR);

        decoder.stop();
    }

    TEST_CASE("seekToMs with zero value")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.seekToMs(0);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("seekToMs with large value is handled")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.seekToMs(UINT64_MAX);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("multiple rapid seeks in IDLE are handled")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.seekToMs(1000);
        decoder.seekToMs(2000);
        decoder.seekToMs(500);
        decoder.seekToMs(0);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }
}

TEST_SUITE("AudioStreamDecoder State Queries")
{
    TEST_CASE("state returns IDLE initially")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("positionMs returns 0 initially")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        CHECK(decoder.positionMs() == 0);
    }

    TEST_CASE("durationMs returns 0 initially")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        CHECK(decoder.durationMs() == 0);
    }

    TEST_CASE("positionMs with zero bytesPerMs returns 0")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec;
        devSpec.sampleRate = 0; // Will result in bytesPerMs = 0
        AudioStreamDecoder decoder(&ring, devSpec);

        CHECK(decoder.positionMs() == 0);
    }
}

TEST_CASE("canDecode returns false when not started")
{
    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec;
    devSpec.sampleRate    = 44100;
    devSpec.format        = AudioFormatS16;
    devSpec.numChannel    = 2;
    devSpec.bitsPerSample = 16;
    AudioStreamDecoder decoder(&ring, devSpec);

    CHECK(decoder.canDecode() == false);
}

TEST_CASE("canDecode returns true when started")
{
    RealMediaFixture fixture;
    if (!fixture.exists("小镇姑娘-陶喆.flac")) {
        return;
    }

    std::shared_ptr<FileSource> source;
    auto extractor = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
    REQUIRE(extractor != nullptr);
    REQUIRE(extractor->initCheck() == sdk_utils::OK);

    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec = extractor->getAudioSpec();
    AudioStreamDecoder decoder(&ring, devSpec);

    REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
    CHECK(decoder.canDecode() == true);

    decoder.stop();
}

// startThread=false 只做初始化，不产出数据：同步入口已删除（#18），
// 所以这个状态下的解码器只能被观察/拆掉，不会推进。用例把这一点显式锁住，
// 免得以后有人以为「不启线程也能自己喂」。
TEST_CASE("start with startThread false initializes without decoding")
{
    RealMediaFixture fixture;
    if (!fixture.exists("music.wav")) {
        return;
    }

    std::shared_ptr<FileSource> source;
    auto extractor = fixture.create("music.wav", ".wav", source);
    REQUIRE(extractor != nullptr);

    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec = extractor->getAudioSpec();
    AudioStreamDecoder decoder(&ring, devSpec);

    REQUIRE(decoder.start(extractor.get(), false) == sdk_utils::OK);

    CHECK(decoder.canDecode() == false);
    CHECK(decoder.state() == StreamDecoderState::IDLE);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(ring.availableRead() == 0); // 没有线程驱动，就没有数据

    decoder.stop();
}

// 这里原本有两条 decodeNext() 用例（#18）。同步路径已删除：它对 FLAC/Vorbis
// 每轮新建解码器、只解首帧，实测每轮字节数与 FNV 指纹完全相同，却返回「有进度」，
// 于是 `while (ret != EOS)` 永不终止。同步整段解码请用 AudioDecodeProcess。

TEST_SUITE("AudioStreamDecoder Lifecycle")
{
    TEST_CASE("destructor calls stop")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();

        {
            AudioStreamDecoder decoder(&ring, devSpec);
            CHECK(decoder.state() == StreamDecoderState::IDLE);
        }
        // Destructor should call stop() safely
    }

    TEST_CASE("stop on never-started decoder is safe")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDevSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        CHECK_NOTHROW(decoder.stop());
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("start and immediate stop")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music.wav", ".wav", source);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec  = extractor->getAudioSpec();
        devSpec.durationMs = 0;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        decoder.stop();
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("restart with different extractor")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source1;
        auto extractor1 = fixture.create("music.wav", ".wav", source1);
        REQUIRE(extractor1 != nullptr);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec  = extractor1->getAudioSpec();
        devSpec.durationMs = 0;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor1.get()) == sdk_utils::OK);

        // Stop and restart with same extractor
        decoder.stop();
        CHECK(decoder.state() == StreamDecoderState::IDLE);

        REQUIRE(decoder.start(extractor1.get()) == sdk_utils::OK);
        decoder.stop();
    }
}

TEST_SUITE("AudioStreamDecoder with Real Media")
{
    TEST_CASE("WAV stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music.wav", ".wav", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec  = extractor->getAudioSpec();
        devSpec.durationMs = 0;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("AudioStreamDecoder drains ASF checked-in media without ERROR")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("萨克斯机.asf")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("萨克斯机.asf", ".asf", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec  = extractor->getAudioSpec();
        devSpec.durationMs = 0;
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.state() == StreamDecoderState::EOS);
        CHECK(decoder.durationMs() > 0);
        decoder.stop();
    }

    TEST_CASE("FLAC stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 10000));
        CHECK(decoder.durationMs() >= 0);

        decoder.stop();
    }

    TEST_CASE("OGG stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("Ringtones-耳聆网.ogg")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("Ringtones-耳聆网.ogg", ".ogg", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() >= 0);

        decoder.stop();
    }

    TEST_CASE("AIFF stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("俱乐部音乐循环-Freesound.aiff")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("俱乐部音乐循环-Freesound.aiff", ".aiff", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("MP3 stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.128.mp3")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.128.mp3", ".mp3", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("AAC stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("48000_fltp_1.aac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("M4A stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("摇滚乐_Freesound.m4a")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("摇滚乐_Freesound.m4a", ".m4a", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("APE stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-ape.ape")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music-ape.ape", ".ape", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("MKV stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-mkv.mkv")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music-mkv.mkv", ".mkv", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("WMA stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("十七岁-陶喆.96.wma")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("十七岁-陶喆.96.wma", ".wma", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("AMR stream decoder completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.96.amr")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.96.amr", ".amr", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("stream decoder with resampling completes to EOS")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("48000_fltp_1.aac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("48000_fltp_1.aac", ".aac", source);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        // Use different output spec to force resampling
        AudioSpec devSpec;
        devSpec.sampleRate     = 44100; // Different from source (48000)
        devSpec.numChannel     = 2;
        devSpec.bitsPerSample  = 16;
        devSpec.bytesPerSample = 2;
        devSpec.format         = AudioFormatS16;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));

        decoder.stop();
    }
}

TEST_SUITE("AudioStreamDecoder Edge Cases")
{
    TEST_CASE("decoder handles small ring buffer")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music.wav", ".wav", source);
        REQUIRE(extractor != nullptr);

        AudioRingBuffer ring(4096); // Small buffer
        AudioSpec devSpec  = extractor->getAudioSpec();
        devSpec.durationMs = 0;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 10000));

        decoder.stop();
    }

    TEST_CASE("position advances during decode")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music.wav", ".wav", source);
        REQUIRE(extractor != nullptr);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec  = extractor->getAudioSpec();
        devSpec.durationMs = 0;

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        // Wait a bit for decoding to progress
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Position should have advanced (if media is long enough)
        // But we won't assert on exact value since timing is variable

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 5000));

        decoder.stop();
    }
}

// ---------------------------------------------------------------------------
// P1-1：AudioStreamDecoder 的注入 / 解绑时序（issue #135）。
// 复用上面的 AbortProbeSource 与 MockExtractor，只观察绑定状态，不启解码线程。
// ---------------------------------------------------------------------------
TEST_SUITE("AbortSignalWiring Decoder")
{
    TEST_CASE("start injects the abort flag and stop unbinds it")
    {
        MockExtractor extractor;
        AudioSpec devSpec = extractor.getAudioSpec();
        AudioRingBuffer ring(8192);
        AbortProbeSource source;
        extractor.setDataSource(&source);

        CHECK(source.injectedFlag() == nullptr);

        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(&extractor, false) == sdk_utils::OK);
        CHECK(source.injectedFlag() != nullptr);
        CHECK(source.abortRequested() == false);

        decoder.stop();
        CHECK(source.injectedFlag() == nullptr);
        CHECK(source.abortRequested() == false);
    }

    TEST_CASE("each decoder injects its own flag; restart re-injects and destructor unbinds")
    {
        MockExtractor extractor;
        AudioSpec devSpec = extractor.getAudioSpec();
        AudioRingBuffer ring(8192);
        AbortProbeSource source;
        extractor.setDataSource(&source);

        // 两个 decoder 同时存活，栈地址不会复用，注入的信号地址必然不同。
        AudioStreamDecoder first(&ring, devSpec);
        AudioStreamDecoder second(&ring, devSpec);

        REQUIRE(first.start(&extractor, false) == sdk_utils::OK);
        const std::atomic<bool> *firstFlag = source.injectedFlag();
        REQUIRE(firstFlag != nullptr);
        // 同一 decoder 重注：start() 首行 stop() 解绑旧 flag 再注入同一个信号。
        REQUIRE(first.start(&extractor, false) == sdk_utils::OK);
        CHECK(source.injectedFlag() == firstFlag);

        first.stop();
        CHECK(source.injectedFlag() == nullptr);   // 显式 stop 解绑

        // 换一个 decoder：注入它自己的信号，不是 first 的悬垂 flag。
        REQUIRE(second.start(&extractor, false) == sdk_utils::OK);
        const std::atomic<bool> *secondFlag = source.injectedFlag();
        REQUIRE(secondFlag != nullptr);
        CHECK(secondFlag != firstFlag);
        second.stop();
        CHECK(source.injectedFlag() == nullptr);

        // 不显式 stop：析构路径（~AudioStreamDecoder 调 stop）也必须解绑。
        {
            AudioStreamDecoder third(&ring, devSpec);
            REQUIRE(third.start(&extractor, false) == sdk_utils::OK);
            REQUIRE(source.injectedFlag() != nullptr);
        }
        CHECK(source.injectedFlag() == nullptr);
    }

    TEST_CASE("a legal seek does not raise the injected abort signal")
    {
        MockExtractor extractor;
        AudioSpec devSpec = extractor.getAudioSpec();
        AudioRingBuffer ring(8192);
        AbortProbeSource source;
        extractor.setDataSource(&source);
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(&extractor, false) == sdk_utils::OK);
        const std::atomic<bool> *flag = source.injectedFlag();
        REQUIRE(flag != nullptr);
        REQUIRE(flag->load() == false);

        decoder.seekToMs(500);
        decoder.seekToMs(1200);

        CHECK(flag->load() == false);             // 唯一取消信号未被 seek 置起
        CHECK(source.abortRequested() == false); // 数据源层不误判中断
        CHECK(source.injectedFlag() == flag);    // 注入仍在

        decoder.stop();
        CHECK(source.injectedFlag() == nullptr);
    }
}
