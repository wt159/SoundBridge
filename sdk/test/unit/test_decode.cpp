#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecode.h>
#include <AudioDecodeProcess.h>
#include <AudioStreamDecoder.h>
#include <FLACDecode.h>
#include <VorbisDecode.h>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

class ChunkedDecodeCallback : public AudioDecodeCallback {
public:
    std::vector<AudioBuffer::AudioBufferPtr> &frames;
    AudioSpec spec;

    ChunkedDecodeCallback(std::vector<AudioBuffer::AudioBufferPtr> &outFrames)
        : frames(outFrames)
    {
    }

    void onAudioDecodeCallback(AudioDecodeSpec &out) override
    {
        if (out.spec.samples == 0 || out.lineData == nullptr) {
            return;
        }

        size_t frameSize = static_cast<size_t>(out.spec.samples)
            * static_cast<size_t>(out.spec.numChannel)
            * static_cast<size_t>(out.spec.bytesPerSample);

        auto buf = std::make_shared<AudioBuffer>(frameSize);

        size_t offset = 0;
        for (size_t i = 0; i < static_cast<size_t>(out.spec.samples); ++i) {
            for (int ch = 0; ch < out.spec.numChannel; ++ch) {
                std::memcpy(buf->data() + offset,
                            out.lineData[ch] + static_cast<size_t>(out.spec.bytesPerSample) * i,
                            static_cast<size_t>(out.spec.bytesPerSample));
                offset += static_cast<size_t>(out.spec.bytesPerSample);
            }
        }

        frames.push_back(buf);
        spec = out.spec;
    }
};

class TestProcessExtractor : public ExtractorHelper {
public:
    TestProcessExtractor(AudioCodecID codecId, const AudioSpec &spec,
                         const AudioBuffer::AudioBufferPtr &metaData,
                         DataSourceBase *dataSource = nullptr)
        : m_codecId(codecId)
        , m_spec(spec)
        , m_metaData(metaData)
        , m_dataSource(dataSource)
    {
    }

    sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return m_codecId; }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_metaData; }
    DataSourceBase *getDataSource() override { return m_dataSource; }
    off64_t getDataSize() override { return 0; }
    off64_t getAudioDataOffset() override { return 0; }

private:
    AudioCodecID m_codecId;
    AudioSpec m_spec;
    AudioBuffer::AudioBufferPtr m_metaData;
    DataSourceBase *m_dataSource;
};

std::vector<char> loadBinaryFile(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return std::vector<char>();
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0) {
        return std::vector<char>();
    }

    std::vector<char> data(static_cast<size_t>(size));
    input.read(data.data(), size);
    if (!input) {
        return std::vector<char>();
    }
    return data;
}

std::vector<char> loadRealMediaBytes(const RealMediaFixture &fixture, const std::string &fileName);
std::vector<char> loadRealMediaBytes(const RealMediaFixture &fixture, const std::string &fileName)
{
    return loadBinaryFile(fixture.mediaPath(fileName));
}

std::unique_ptr<ExtractorHelper>
createRealMediaExtractorForProcess(const RealMediaFixture &fixture, const std::string &fileName,
                                   const std::string &extension,
                                   std::shared_ptr<FileSource> &source)
{
    return fixture.create(fileName, extension, source);
}

bool waitForDecoderState(AudioStreamDecoder &decoder, StreamDecoderState expectedState,
                         int timeoutMs)
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

AudioSpec createStreamDecoderDeviceSpec(ExtractorHelper *extractor);
AudioSpec createStreamDecoderDeviceSpec(ExtractorHelper *extractor)
{
    AudioSpec spec  = extractor->getAudioSpec();
    spec.durationMs = 0;
    return spec;
}

bool verifyIdleDecoderState(AudioStreamDecoder &decoder)
{
    return decoder.state() == StreamDecoderState::IDLE && decoder.positionMs() == 0
        && decoder.durationMs() == 0;
}

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

    TEST_CASE("Decode AAC checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        TestDecodeCallback callback;
        AudioDecode decode(AUDIO_CODEC_ID_AAC, &callback);
        REQUIRE(decode.initCheck() == sdk_utils::OK);

        const std::vector<char> input = loadBinaryFile(fixture.mediaPath("48000_fltp_1.aac"));
        REQUIRE(input.empty() == false);

        const int result = decode.decode(input.data(), static_cast<int>(input.size()));
        CHECK(result >= 0);
        CHECK(callback.decodeCount > 0);
        CHECK(callback.lastSpec.spec.sampleRate > 0);
        CHECK(callback.lastSpec.spec.numChannel > 0);
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

    TEST_CASE("Process FLAC checked-in media through extractor")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.flac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "小镇姑娘-陶喆.flac", ".flac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioDecodeProcess process(extractor.get());
        REQUIRE(process.initCheck() == sdk_utils::OK);
        REQUIRE(process.getDecodeBuffer() != nullptr);

        const AudioSpec spec = process.getDecodeSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
    }

    TEST_CASE("Process OGG checked-in media through extractor")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("Ringtones-耳聆网.ogg"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "Ringtones-耳聆网.ogg", ".ogg", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioDecodeProcess process(extractor.get());
        REQUIRE(process.initCheck() == sdk_utils::OK);
        REQUIRE(process.getDecodeBuffer() != nullptr);

        const AudioSpec spec = process.getDecodeSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs >= 0);
    }

    TEST_CASE("Process AAC checked-in media through extractor")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioDecodeProcess process(extractor.get());
        REQUIRE(process.initCheck() == sdk_utils::OK);
        REQUIRE(process.getDecodeBuffer() != nullptr);

        const AudioSpec spec = process.getDecodeSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
    }

    TEST_CASE("Process WAV checked-in media through extractor")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "music.wav", ".wav", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        REQUIRE(extractor->getAudioCodecID() == AUDIO_CODEC_ID_NONE);

        AudioDecodeProcess process(extractor.get());
        REQUIRE(process.initCheck() == sdk_utils::OK);
        REQUIRE(process.getDecodeBuffer() != nullptr);

        const AudioSpec spec = process.getDecodeSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
    }

    TEST_CASE("Process none codec with null metadata returns invalid operation")
    {
        AudioSpec spec;
        spec.sampleRate     = 16000;
        spec.numChannel     = 1;
        spec.bytesPerSample = 2;
        spec.bitsPerSample  = 16;
        spec.durationMs     = 1;

        TestProcessExtractor extractor(AUDIO_CODEC_ID_NONE, spec, nullptr);
        AudioDecodeProcess process(&extractor);

        REQUIRE(process.initCheck() == sdk_utils::INVALID_OPERATION);
        REQUIRE(process.getDecodeBuffer() == nullptr);
    }

    TEST_CASE("Process FLAC codec with null metadata and no source returns invalid operation")
    {
        AudioSpec spec;
        spec.sampleRate     = 44100;
        spec.numChannel     = 2;
        spec.bytesPerSample = 2;
        spec.bitsPerSample  = 16;
        spec.durationMs     = 1;

        TestProcessExtractor extractor(AUDIO_CODEC_ID_FLAC, spec, nullptr);
        AudioDecodeProcess process(&extractor);

        REQUIRE(process.initCheck() == sdk_utils::INVALID_OPERATION);
        REQUIRE(process.getDecodeBuffer() == nullptr);
    }

    TEST_CASE("Process Vorbis codec with null metadata and no source returns invalid operation")
    {
        AudioSpec spec;
        spec.sampleRate     = 44100;
        spec.numChannel     = 2;
        spec.bytesPerSample = 2;
        spec.bitsPerSample  = 16;
        spec.durationMs     = 1;

        TestProcessExtractor extractor(AUDIO_CODEC_ID_VORBIS, spec, nullptr);
        AudioDecodeProcess process(&extractor);

        REQUIRE(process.initCheck() == sdk_utils::INVALID_OPERATION);
        REQUIRE(process.getDecodeBuffer() == nullptr);
    }

    TEST_CASE("Process generic codec with null metadata returns invalid operation")
    {
        AudioSpec spec;
        spec.sampleRate     = 48000;
        spec.numChannel     = 1;
        spec.bytesPerSample = 2;
        spec.bitsPerSample  = 16;
        spec.durationMs     = 1;

        TestProcessExtractor extractor(AUDIO_CODEC_ID_AAC, spec, nullptr);
        AudioDecodeProcess process(&extractor);

        REQUIRE(process.initCheck() == sdk_utils::INVALID_OPERATION);
        REQUIRE(process.getDecodeBuffer() == nullptr);
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

    TEST_CASE("Decode FLAC checked-in media through data source")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.flac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        REQUIRE(extractor->getDataSource() != nullptr);

        TestDecodeCallback callback;
        FLACDecode decode(&callback);
        REQUIRE(decode.initFromDataSource(
            extractor->getDataSource(), extractor->getAudioDataOffset(), extractor->getDataSize()));

        int result = 1;
        while (result > 0) {
            result = decode.processOne();
        }

        CHECK(result == 0);
        CHECK(callback.decodeCount > 0);
        CHECK(callback.lastSpec.spec.sampleRate > 0);
        CHECK(callback.lastSpec.spec.numChannel > 0);
        CHECK(callback.lastSpec.spec.bitsPerSample > 0);
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

    TEST_CASE("Decode OGG checked-in media through data source")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("Ringtones-耳聆网.ogg"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("Ringtones-耳聆网.ogg", ".ogg", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        REQUIRE(extractor->getDataSource() != nullptr);

        TestDecodeCallback callback;
        VorbisDecode decode(&callback);
        REQUIRE(decode.initVFFromSource(extractor->getDataSource(), extractor->getDataSize()));

        int result = 1;
        while (result > 0) {
            result = decode.decodeOne();
        }

        CHECK(result == 0);
        CHECK(callback.decodeCount > 0);
        CHECK(callback.lastSpec.spec.sampleRate > 0);
        CHECK(callback.lastSpec.spec.numChannel > 0);
        CHECK(callback.lastSpec.spec.bitsPerSample == 16);
    }
}

TEST_SUITE("AudioStreamDecoder")
{
    TEST_CASE("Start with null extractor returns invalid operation")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        const sdk_utils::status_t result = decoder.start(nullptr);

        REQUIRE(result == sdk_utils::INVALID_OPERATION);
        REQUIRE(decoder.state() == StreamDecoderState::ERROR);
    }

    TEST_CASE("Start with null ring returns invalid operation")
    {
        AudioSpec devSpec;
        AudioStreamDecoder decoder(nullptr, devSpec);

        std::shared_ptr<FileSource> source;
        RealMediaFixture fixture;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "music.wav", ".wav", source);
        REQUIRE(extractor != nullptr);

        const sdk_utils::status_t result = decoder.start(extractor.get());

        REQUIRE(result == sdk_utils::INVALID_OPERATION);
        REQUIRE(decoder.state() == StreamDecoderState::ERROR);
    }

    TEST_CASE("Idle stream decoder position is zero")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.positionMs() == 0);
        REQUIRE(decoder.durationMs() == 0);
    }

    TEST_CASE("Seek on idle stream decoder keeps idle state")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.seekToMs(1234);

        REQUIRE(verifyIdleDecoderState(decoder));
    }

    TEST_CASE("Decode AAC checked-in media through stream decoder")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 3000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("Stream decoder seek while decoding")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        REQUIRE(decoder.state() == StreamDecoderState::DECODING);

        decoder.seekToMs(1000);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 3000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("Stream decoder position while decoding")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        REQUIRE(decoder.positionMs() == 0);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 3000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("Stream decoder seek in IDLE state does nothing")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.state() == StreamDecoderState::IDLE);
        decoder.seekToMs(1000);
        CHECK(decoder.state() == StreamDecoderState::IDLE);
    }

    TEST_CASE("Stream decoder seek in ERROR state does nothing")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        decoder.start(nullptr);
        REQUIRE(decoder.state() == StreamDecoderState::ERROR);

        decoder.seekToMs(1000);
        CHECK(decoder.state() == StreamDecoderState::ERROR);

        decoder.stop();
    }

    TEST_CASE("Stream decoder position with zero bytesPerMs returns 0")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
        REQUIRE(decoder.state() == StreamDecoderState::DECODING);

        decoder.stop();
        CHECK(decoder.positionMs() == 0);
    }

    TEST_CASE("Stream decoder seek to zero position")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = extractor->getAudioSpec();
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);

        decoder.seekToMs(0);

        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 3000));
        CHECK(decoder.durationMs() > 0);

        decoder.stop();
    }

    TEST_CASE("Stream decoder positionMs before start has zero bytesPerMs")
    {
        AudioRingBuffer ring(1024);
        AudioSpec devSpec;
        AudioStreamDecoder decoder(&ring, devSpec);

        REQUIRE(decoder.state() == StreamDecoderState::IDLE);
        REQUIRE(decoder.positionMs() == 0);
    }

    TEST_CASE("Stream decoder FLAC stream to EOS")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.flac"));

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = createRealMediaExtractorForProcess(fixture, "小镇姑娘-陶喆.flac", ".flac", source);
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
}

TEST_CASE("Decode WAV checked-in media through stream decoder")
{
    RealMediaFixture fixture;
    REQUIRE(fixture.exists("music.wav"));

    std::shared_ptr<FileSource> source;
    std::unique_ptr<ExtractorHelper> extractor
        = createRealMediaExtractorForProcess(fixture, "music.wav", ".wav", source);
    REQUIRE(source != nullptr);
    REQUIRE(extractor != nullptr);
    REQUIRE(extractor->initCheck() == sdk_utils::OK);
    REQUIRE(extractor->getAudioCodecID() == AUDIO_CODEC_ID_NONE);

    AudioRingBuffer ring(1 << 20);
    AudioSpec devSpec = createStreamDecoderDeviceSpec(extractor.get());
    AudioStreamDecoder decoder(&ring, devSpec);

    REQUIRE(decoder.start(extractor.get()) == sdk_utils::OK);
    REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 3000));
    CHECK(decoder.durationMs() > 0);

    decoder.stop();
}

TEST_CASE("Chunked decode consistency with full file decode - AAC")
{
    RealMediaFixture fixture;
    REQUIRE(fixture.exists("48000_fltp_1.aac"));

    std::shared_ptr<FileSource> source;
    std::unique_ptr<ExtractorHelper> extractor
        = createRealMediaExtractorForProcess(fixture, "48000_fltp_1.aac", ".aac", source);
    REQUIRE(source != nullptr);
    REQUIRE(extractor != nullptr);
    REQUIRE(extractor->initCheck() == sdk_utils::OK);

    AudioDecodeProcess processFull(extractor.get());
    REQUIRE(processFull.initCheck() == sdk_utils::OK);
    auto bufferFull = processFull.getDecodeBuffer();
    REQUIRE(bufferFull != nullptr);
    REQUIRE(bufferFull->size() > 0);

    AudioBuffer::AudioBufferPtr metaData = extractor->getMetaData();
    const char *data                     = metaData->data();
    size_t totalSize                     = metaData->size();

    std::vector<AudioBuffer::AudioBufferPtr> chunkFrames;

    ChunkedDecodeCallback callback(chunkFrames);

    AudioDecode decode(AUDIO_CODEC_ID_AAC, &callback);
    REQUIRE(decode.initCheck() == sdk_utils::OK);

    int ret = decode.decode(data, static_cast<ssize_t>(totalSize));
    if (ret < 0) {
        FAIL("Single call decode failed ret=" << ret);
    }

    ret = decode.decode(nullptr, 0);
    if (ret < 0) {
        FAIL("Flush decode failed");
    }

    AudioBuffer::AudioBufferPtr bufferChunked;
    {
        size_t mergedSize = 0;
        for (auto &buf : chunkFrames) {
            mergedSize += buf->size();
        }
        bufferChunked  = std::make_shared<AudioBuffer>(mergedSize);
        off64_t offset = 0;
        for (auto &buf : chunkFrames) {
            bufferChunked->setData(offset, buf->size(), buf->data());
            offset += buf->size();
        }
    }
    REQUIRE(bufferChunked != nullptr);
    REQUIRE(bufferChunked->size() > 0);

    CHECK(bufferFull->size() == bufferChunked->size());
    CHECK(std::memcmp(bufferFull->data(), bufferChunked->data(), bufferFull->size()) == 0);
}

TEST_CASE("Chunked decode consistency with full file decode - MP3")
{
    RealMediaFixture fixture;
    REQUIRE(fixture.exists("小镇姑娘-陶喆.128.mp3"));

    std::shared_ptr<FileSource> source;
    std::unique_ptr<ExtractorHelper> extractor
        = createRealMediaExtractorForProcess(fixture, "小镇姑娘-陶喆.128.mp3", ".mp3", source);
    REQUIRE(source != nullptr);
    REQUIRE(extractor != nullptr);
    REQUIRE(extractor->initCheck() == sdk_utils::OK);

    AudioDecodeProcess processFull(extractor.get());
    REQUIRE(processFull.initCheck() == sdk_utils::OK);
    auto bufferFull = processFull.getDecodeBuffer();
    REQUIRE(bufferFull != nullptr);
    REQUIRE(bufferFull->size() > 0);

    AudioBuffer::AudioBufferPtr metaData = extractor->getMetaData();
    const char *data                     = metaData->data();
    size_t totalSize                     = metaData->size();

    std::vector<AudioBuffer::AudioBufferPtr> chunkFrames;

    ChunkedDecodeCallback callback(chunkFrames);

    AudioDecode decode(AUDIO_CODEC_ID_MP3, &callback);
    REQUIRE(decode.initCheck() == sdk_utils::OK);

    int ret = decode.decode(data, static_cast<ssize_t>(totalSize));
    if (ret < 0) {
        FAIL("Single call decode failed ret=" << ret);
    }

    ret = decode.decode(nullptr, 0);
    if (ret < 0) {
        FAIL("Flush decode failed");
    }

    AudioBuffer::AudioBufferPtr bufferChunked;
    {
        size_t mergedSize = 0;
        for (auto &buf : chunkFrames) {
            mergedSize += buf->size();
        }
        bufferChunked  = std::make_shared<AudioBuffer>(mergedSize);
        off64_t offset = 0;
        for (auto &buf : chunkFrames) {
            bufferChunked->setData(offset, buf->size(), buf->data());
            offset += buf->size();
        }
    }
    REQUIRE(bufferChunked != nullptr);
    REQUIRE(bufferChunked->size() > 0);

    CHECK(bufferFull->size() == bufferChunked->size());
    CHECK(std::memcmp(bufferFull->data(), bufferChunked->data(), bufferFull->size()) == 0);
}

TEST_CASE("Chunked decode consistency with full file decode - M4A")
{
    RealMediaFixture fixture;
    REQUIRE(fixture.exists("摇滚乐_Freesound.m4a"));

    std::shared_ptr<FileSource> source;
    std::unique_ptr<ExtractorHelper> extractor
        = createRealMediaExtractorForProcess(fixture, "摇滚乐_Freesound.m4a", ".m4a", source);
    REQUIRE(source != nullptr);
    REQUIRE(extractor != nullptr);
    REQUIRE(extractor->initCheck() == sdk_utils::OK);

    AudioDecodeProcess processFull(extractor.get());
    REQUIRE(processFull.initCheck() == sdk_utils::OK);
    auto bufferFull = processFull.getDecodeBuffer();
    REQUIRE(bufferFull != nullptr);
    REQUIRE(bufferFull->size() > 0);

    AudioBuffer::AudioBufferPtr metaData = extractor->getMetaData();
    const char *data                     = metaData->data();
    size_t totalSize                     = metaData->size();

    std::vector<AudioBuffer::AudioBufferPtr> chunkFrames;

    ChunkedDecodeCallback callback(chunkFrames);

    AudioDecode decode(AUDIO_CODEC_ID_AAC, &callback);
    REQUIRE(decode.initCheck() == sdk_utils::OK);

    int ret = decode.decode(data, static_cast<ssize_t>(totalSize));
    if (ret < 0) {
        FAIL("Single call decode failed ret=" << ret);
    }

    ret = decode.decode(nullptr, 0);
    if (ret < 0) {
        FAIL("Flush decode failed");
    }

    AudioBuffer::AudioBufferPtr bufferChunked;
    {
        size_t mergedSize = 0;
        for (auto &buf : chunkFrames) {
            mergedSize += buf->size();
        }
        bufferChunked  = std::make_shared<AudioBuffer>(mergedSize);
        off64_t offset = 0;
        for (auto &buf : chunkFrames) {
            bufferChunked->setData(offset, buf->size(), buf->data());
            offset += buf->size();
        }
    }
    REQUIRE(bufferChunked != nullptr);
    REQUIRE(bufferChunked->size() > 0);

    CHECK(bufferFull->size() == bufferChunked->size());
    CHECK(std::memcmp(bufferFull->data(), bufferChunked->data(), bufferFull->size()) == 0);
}
