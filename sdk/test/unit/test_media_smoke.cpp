#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecodeProcess.h>
#include <ExtractorFactory.h>
#include <FileSource.h>
#include <doctest/doctest.h>

TEST_SUITE("MediaSmoke")
{
    TEST_CASE("WAV media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("music.wav", ".wav", "MediaSmoke wav", source, extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("FLAC media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("小镇姑娘-陶喆.flac", ".flac", "MediaSmoke flac", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("MP3 media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.128.mp3")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("小镇姑娘-陶喆.128.mp3", ".mp3", "MediaSmoke mp3", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("AAC media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("48000_fltp_1.aac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("48000_fltp_1.aac", ".aac", "MediaSmoke aac", source, extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("OGG media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("Ringtones-耳聆网.ogg")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("Ringtones-耳聆网.ogg", ".ogg", "MediaSmoke ogg", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
    }

    TEST_CASE("M4A media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("摇滚乐_Freesound.m4a")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("摇滚乐_Freesound.m4a", ".m4a", "MediaSmoke m4a", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
    }

    TEST_CASE("AIFF media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("俱乐部音乐循环-Freesound.aiff")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("俱乐部音乐循环-Freesound.aiff", ".aiff", "MediaSmoke aiff",
                                source, extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);
        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("ASF media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("萨克斯机.asf")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("萨克斯机.asf", ".asf", "MediaSmoke asf", source, extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("APE media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-ape.ape")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("music-ape.ape", ".ape", "MediaSmoke ape", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);
        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("MKV media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-mkv.mkv")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("music-mkv.mkv", ".mkv", "MediaSmoke mkv", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);
        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("WMA media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("十七岁-陶喆.96.wma")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("十七岁-陶喆.96.wma", ".wma", "MediaSmoke wma", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);

        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }

    TEST_CASE("AMR media smoke test")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.96.amr")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor;
        if (!fixture.openOrSkip("小镇姑娘-陶喆.96.amr", ".amr", "MediaSmoke amr", source,
                                extractor)) {
            return;
        }

        REQUIRE(extractor->initCheck() == sdk_utils::OK);
        std::shared_ptr<AudioDecodeProcess> decode(new AudioDecodeProcess(extractor.get()));
        REQUIRE(decode->initCheck() == sdk_utils::OK);
        AudioBuffer::AudioBufferPtr buffer = decode->getDecodeBuffer();
        REQUIRE(buffer != nullptr);
        CHECK(buffer->size() > 0);
    }
}
