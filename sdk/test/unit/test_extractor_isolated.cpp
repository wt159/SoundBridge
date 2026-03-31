#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <ExtractorFactory.h>
#include <FileSource.h>
#include <doctest/doctest.h>

TEST_SUITE("ExtractorIsolated")
{
    TEST_CASE("WAV extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music.wav")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor = fixture.create("music.wav", ".wav", source);

        REQUIRE(source != nullptr);
        REQUIRE(source->initCheck() == sdk_utils::OK);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
    }

    TEST_CASE("AIFF extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("俱乐部音乐循环-Freesound.aiff")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("俱乐部音乐循环-Freesound.aiff", ".aiff", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("FLAC extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.flac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("MP3 extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.128.mp3")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("小镇姑娘-陶喆.128.mp3", ".mp3", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("OGG extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("Ringtones-耳聆网.ogg")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("Ringtones-耳聆网.ogg", ".ogg", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("AAC extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("48000_fltp_1.aac")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("48000_fltp_1.aac", ".aac", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("M4A extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("摇滚乐_Freesound.m4a")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("摇滚乐_Freesound.m4a", ".m4a", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("ASF extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("萨克斯机.asf")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor = fixture.create("萨克斯机.asf", ".asf", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("APE extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-ape.ape")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("music-ape.ape", ".ape", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("MKV extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("music-mkv.mkv")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("music-mkv.mkv", ".mkv", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("WMA extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("十七岁-陶喆.96.wma")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("十七岁-陶喆.96.wma", ".wma", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("AMR extractor initialization and lifecycle")
    {
        RealMediaFixture fixture;
        if (!fixture.exists("小镇姑娘-陶喆.96.amr")) {
            return;
        }

        std::shared_ptr<FileSource> source;
        std::unique_ptr<ExtractorHelper> extractor
            = fixture.create("小镇姑娘-陶喆.96.amr", ".amr", source);

        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }
}
