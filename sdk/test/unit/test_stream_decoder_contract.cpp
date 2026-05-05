#define private public
#include <AudioStreamDecoder.h>
#undef private

#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioRingBuffer.h>
#include <ExtractorHelper.hpp>
#include <doctest/doctest.h>

namespace {

AudioSpec makeDeviceSpec()
{
    AudioSpec spec;
    spec.sampleRate     = 44100;
    spec.numChannel     = 2;
    spec.bitsPerSample  = 16;
    spec.bytesPerSample = 2;
    spec.format         = AudioFormatS16;
    return spec;
}

class ContractExtractor : public ExtractorHelper {
public:
    ContractExtractor(AudioCodecID codecId, const AudioSpec &spec)
        : m_codecId(codecId)
        , m_spec(spec)
        , m_metaData(std::make_shared<AudioBuffer>(16))
    {
    }

    sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
    AudioSpec getAudioSpec() override { return m_spec; }
    AudioCodecID getAudioCodecID() override { return m_codecId; }
    AudioBuffer::AudioBufferPtr getMetaData() override { return m_metaData; }
    DataSourceBase *getDataSource() override { return nullptr; }
    off64_t getDataSize() override { return m_metaData ? static_cast<off64_t>(m_metaData->size()) : 0; }
    off64_t getAudioDataOffset() override { return 0; }

private:
    AudioCodecID m_codecId;
    AudioSpec m_spec;
    AudioBuffer::AudioBufferPtr m_metaData;
};

class DecoderHarness : public AudioStreamDecoder {
public:
    DecoderHarness(AudioRingBuffer *ring, const AudioSpec &devSpec)
        : AudioStreamDecoder(ring, devSpec)
    {
    }

    using AudioStreamDecoder::onAudioDecodeCallback;
};

} // namespace

TEST_SUITE("AudioStreamDecoder format trust policy")
{
    TEST_CASE("generic ffmpeg codec defers guessed pcm format but keeps bits per sample")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDeviceSpec();
        DecoderHarness decoder(&ring, devSpec);

        AudioSpec guessed = devSpec;
        ContractExtractor extractor(AUDIO_CODEC_ID_AAC, guessed);

        REQUIRE(decoder.start(&extractor, false) == sdk_utils::OK);
        CHECK(decoder.m_srcSpec.sampleRate == guessed.sampleRate);
        CHECK(decoder.m_srcSpec.numChannel == guessed.numChannel);
        CHECK(decoder.m_srcSpec.format == AudioFormatUnknown);
        CHECK(decoder.m_srcSpec.bytesPerSample == 0);
        CHECK(decoder.m_srcSpec.bitsPerSample == guessed.bitsPerSample);
        REQUIRE(decoder.m_lazyResample.isInit());
        CHECK(decoder.m_lazyResample->IsValueCreated() == false);
    }

    TEST_CASE("trusted pcm extractor keeps extractor audio format")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDeviceSpec();
        DecoderHarness decoder(&ring, devSpec);

        AudioSpec guessed = devSpec;
        ContractExtractor extractor(AUDIO_CODEC_ID_NONE, guessed);

        REQUIRE(decoder.start(&extractor, false) == sdk_utils::OK);
        CHECK(decoder.m_srcSpec.format == guessed.format);
        CHECK(decoder.m_srcSpec.bytesPerSample == guessed.bytesPerSample);
        CHECK(decoder.m_srcSpec.bitsPerSample == guessed.bitsPerSample);
        CHECK(decoder.m_lazyResample.isInit() == false);
    }

    TEST_CASE("first decode callback promotes actual pcm format for generic ffmpeg codec")
    {
        AudioRingBuffer ring(8192);
        AudioSpec devSpec = makeDeviceSpec();
        DecoderHarness decoder(&ring, devSpec);

        AudioSpec guessed = devSpec;
        ContractExtractor extractor(AUDIO_CODEC_ID_AAC, guessed);
        REQUIRE(decoder.start(&extractor, false) == sdk_utils::OK);

        float sample = 0.0f;
        uint8_t *lineData[] = { reinterpret_cast<uint8_t *>(&sample) };
        int lineSizes[]     = { static_cast<int>(sizeof(sample)) };

        AudioDecodeSpec out;
        out.spec.sampleRate     = 48000;
        out.spec.numChannel     = 1;
        out.spec.bitsPerSample  = 32;
        out.spec.bytesPerSample = 4;
        out.spec.format         = AudioFormatFLT32;
        out.spec.samples        = 1;
        out.lineData            = lineData;
        out.lineSize            = lineSizes;

        decoder.onAudioDecodeCallback(out);

        CHECK(decoder.m_srcSpec.sampleRate == 48000);
        CHECK(decoder.m_srcSpec.numChannel == 1);
        CHECK(decoder.m_srcSpec.format == AudioFormatFLT32);
        CHECK(decoder.m_srcSpec.bytesPerSample == 4);
        CHECK(decoder.m_srcSpec.bitsPerSample == 32);
    }
}
