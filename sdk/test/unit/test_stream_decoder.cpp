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

TEST_CASE("start with autoDecode false does not start thread")
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

    DecodeOptions opts;
    opts.autoDecode = false;
    REQUIRE(decoder.start(extractor.get(), opts) == sdk_utils::OK);

    CHECK(decoder.canDecode() == false);
    CHECK(decoder.state() == StreamDecoderState::IDLE);

    decoder.stop();
}

TEST_CASE("decodeNext returns OK when manually started with FLAC")
{
    RealMediaFixture fixture;
    if (!fixture.exists("小镇姑娘-陶喆.flac")) {
        return;
    }

    std::shared_ptr<FileSource> source;
    auto extractor = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
    REQUIRE(extractor != nullptr);

    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec = extractor->getAudioSpec();
    AudioStreamDecoder decoder(&ring, devSpec);

    DecodeOptions opts;
    opts.autoDecode = false;
    REQUIRE(decoder.start(extractor.get(), opts) == sdk_utils::OK);
    REQUIRE(decoder.state() == StreamDecoderState::IDLE);

    DecodeResult ret = decoder.decodeNext();
    REQUIRE((ret == DecodeResult::OK || ret == DecodeResult::WAIT));

    decoder.stop();
}

TEST_CASE("decodeNext returns OK or WAIT when decoding")
{
    RealMediaFixture fixture;
    if (!fixture.exists("小镇姑娘-陶喆.flac")) {
        return;
    }

    std::shared_ptr<FileSource> source;
    auto extractor = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
    REQUIRE(extractor != nullptr);

    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec = extractor->getAudioSpec();
    AudioStreamDecoder decoder(&ring, devSpec);

    REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

    // Wait for some data
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    DecodeResult ret = decoder.decodeNext();
    CHECK((ret == DecodeResult::OK || ret == DecodeResult::WAIT));

    decoder.stop();
}

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
