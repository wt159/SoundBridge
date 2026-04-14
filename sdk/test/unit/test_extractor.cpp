#include "../fixtures/RealMediaFixture.hpp"
#include <AudioCommon.hpp>
#include <DataSource.hpp>
#include <ExtractorFactory.h>
#include <FileSource.h>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>

TEST_SUITE("DataSourceBase interface")
{
    class MockDataSource : public DataSource {
    public:
        MockDataSource()
            : m_initStatus(sdk_utils::OK)
            , m_size(1024)
        {
        }

        void setInitStatus(sdk_utils::status_t status) { m_initStatus = status; }
        void setSize(off64_t size) { m_size = size; }

        sdk_utils::status_t initCheck() const override { return m_initStatus; }

        ssize_t readAt(off64_t offset, void *data, size_t size) override
        {
            if (offset >= m_size) {
                return -1;
            }
            size_t remain = m_size - offset;
            size_t toRead = (size < remain) ? size : remain;
            if (data) {
                std::memset(data, 0, toRead);
            }
            return toRead;
        }

        sdk_utils::status_t getSize(off64_t *size) override
        {
            if (size) {
                *size = m_size;
            }
            return sdk_utils::OK;
        }

        uint32_t flags() override { return 0; }

        void close() override { }

        sdk_utils::status_t getAvailableSize(off64_t offset, off64_t *size) override
        {
            if (size) {
                *size = m_size - offset;
            }
            return sdk_utils::OK;
        }

        std::string toString() override { return "MockDataSource"; }

        sdk_utils::status_t reconnectAtOffset(off64_t offset) override
        {
            (void)offset;
            return sdk_utils::OK;
        }

        using DataSource::getUri;
        std::string getUri() override { return "mock://test"; }

    private:
        sdk_utils::status_t m_initStatus;
        off64_t m_size;
    };

    TEST_CASE("MockDataSource initialization")
    {
        MockDataSource source;
        REQUIRE(source.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("MockDataSource with error status")
    {
        MockDataSource source;
        source.setInitStatus(sdk_utils::NO_INIT);
        REQUIRE(source.initCheck() == sdk_utils::NO_INIT);
    }

    TEST_CASE("MockDataSource getSize")
    {
        MockDataSource source;
        source.setSize(2048);

        off64_t size               = 0;
        sdk_utils::status_t result = source.getSize(&size);
        REQUIRE(result == sdk_utils::OK);
        REQUIRE(size == 2048);
    }

    TEST_CASE("MockDataSource readAt valid range")
    {
        MockDataSource source;
        source.setSize(1024);

        char buffer[256];
        ssize_t bytesRead = source.readAt(0, buffer, sizeof(buffer));
        REQUIRE(bytesRead == 256);
    }

    TEST_CASE("MockDataSource readAt beyond size")
    {
        MockDataSource source;
        source.setSize(1024);

        char buffer[256];
        ssize_t bytesRead = source.readAt(2000, buffer, sizeof(buffer));
        REQUIRE(bytesRead == -1);
    }

    TEST_CASE("MockDataSource readAt partial read")
    {
        MockDataSource source;
        source.setSize(100);

        char buffer[256];
        ssize_t bytesRead = source.readAt(0, buffer, sizeof(buffer));
        REQUIRE(bytesRead == 100);
    }

    TEST_CASE("MockDataSource flags")
    {
        MockDataSource source;
        REQUIRE(source.flags() == 0);
    }

    TEST_CASE("MockDataSource toString")
    {
        MockDataSource source;
        REQUIRE(source.toString() == "MockDataSource");
    }

    TEST_CASE("MockDataSource getAvailableSize")
    {
        MockDataSource source;
        source.setSize(1024);

        off64_t available          = 0;
        sdk_utils::status_t result = source.getAvailableSize(100, &available);
        REQUIRE(result == sdk_utils::OK);
        REQUIRE(available == 924);
    }

    TEST_CASE("MockDataSource getUri buffer")
    {
        MockDataSource source;
        char buffer[64];
        bool result = source.getUri(buffer, sizeof(buffer));
        REQUIRE(result == true);
        REQUIRE(std::string(buffer) == "mock://test");
    }
}

TEST_SUITE("FileSource")
{
    std::string createUnitFilePath(const std::string &name)
    {
        return std::string("/tmp/") + name;
    }

    std::string createUnitNoExtensionPath(const std::string &name)
    {
        return std::string("/tmp/") + name;
    }

    TEST_CASE("Missing file returns no init")
    {
        FileSource source("/tmp/soundbridge_missing_file_source_case.bin");
        REQUIRE(source.initCheck() == sdk_utils::NO_INIT);
    }

    TEST_CASE("Missing file readAt returns no init")
    {
        FileSource source("/tmp/soundbridge_missing_file_source_case.bin");
        char buffer[8];
        REQUIRE(source.readAt(0, buffer, sizeof(buffer)) == sdk_utils::NO_INIT);
    }

    TEST_CASE("Missing file getSize returns no init")
    {
        FileSource source("/tmp/soundbridge_missing_file_source_case.bin");
        off64_t size = 0;
        REQUIRE(source.getSize(&size) == sdk_utils::NO_INIT);
    }

    TEST_CASE("Negative offset read fails with unknown error")
    {
        const std::string path = createUnitFilePath("file_source_negative_offset.bin");
        {
            std::ofstream output(path.c_str(), std::ios::binary);
            output.write("12345678", 8);
        }

        FileSource source(path.c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);

        char buffer[4];
        REQUIRE(source.readAt(-1, buffer, sizeof(buffer)) == sdk_utils::UNKNOWN_ERROR);

        std::remove(path.c_str());
    }

    TEST_CASE("Null filename returns no init")
    {
        FileSource source(nullptr);
        REQUIRE(source.initCheck() == sdk_utils::NO_INIT);
    }

    TEST_CASE("Path without extension returns no init")
    {
        const std::string path = createUnitNoExtensionPath("file_source_no_extension_case");
        std::remove(path.c_str());

        FileSource source(path.c_str());
        REQUIRE(source.initCheck() == sdk_utils::NO_INIT);
    }
}

TEST_SUITE("ExtractorFactory")
{
    class TestFactoryDataSource : public DataSource {
    public:
        explicit TestFactoryDataSource(const std::vector<uint8_t> &bytes)
            : m_bytes(bytes)
        {
        }

        sdk_utils::status_t initCheck() const override { return sdk_utils::OK; }
        ssize_t readAt(off64_t offset, void *data, size_t size) override
        {
            if (offset < 0 || static_cast<size_t>(offset) >= m_bytes.size()) {
                return -1;
            }
            const size_t remain = m_bytes.size() - static_cast<size_t>(offset);
            const size_t toRead = std::min(remain, size);
            if (data != nullptr && toRead > 0) {
                std::memcpy(data, m_bytes.data() + offset, toRead);
            }
            return static_cast<ssize_t>(toRead);
        }
        sdk_utils::status_t getSize(off64_t *size) override
        {
            if (size != nullptr) {
                *size = static_cast<off64_t>(m_bytes.size());
            }
            return sdk_utils::OK;
        }
        uint32_t flags() override { return 0; }
        void close() override { }
        sdk_utils::status_t getAvailableSize(off64_t offset, off64_t *size) override
        {
            if (size != nullptr) {
                *size = static_cast<off64_t>(m_bytes.size()) - offset;
            }
            return sdk_utils::OK;
        }
        std::string toString() override { return "TestFactoryDataSource"; }
        sdk_utils::status_t reconnectAtOffset(off64_t) override { return sdk_utils::OK; }
        std::string getUri() override { return "test://factory"; }

    private:
        std::vector<uint8_t> m_bytes;
    };

    class TestOnlyExtractor : public ExtractorHelper {
    public:
        explicit TestOnlyExtractor(DataSourceBase *source)
            : m_source(source)
        {
        }

        sdk_utils::status_t initCheck() override { return sdk_utils::OK; }
        AudioSpec getAudioSpec() override { return AudioSpec(); }
        AudioCodecID getAudioCodecID() override { return AUDIO_CODEC_ID_NONE; }
        AudioBuffer::AudioBufferPtr getMetaData() override { return nullptr; }
        DataSourceBase *getDataSource() override { return m_source; }

    private:
        DataSourceBase *m_source;
    };

    bool sniffUnitMagic(DataSourceBase * source)
    {
        if (source == nullptr) {
            return false;
        }

        char header[4] = { 0 };
        return source->readAt(0, header, sizeof(header)) == static_cast<ssize_t>(sizeof(header))
            && std::memcmp(header, "UNIT", sizeof(header)) == 0;
    }

    std::vector<uint8_t> makeMagicHeader(const std::string &kind)
    {
        std::vector<uint8_t> bytes(16, 0);
        if (kind == "wav") {
            bytes[0]  = 'R';
            bytes[1]  = 'I';
            bytes[2]  = 'F';
            bytes[3]  = 'F';
            bytes[8]  = 'W';
            bytes[9]  = 'A';
            bytes[10] = 'V';
            bytes[11] = 'E';
        } else if (kind == "flac") {
            bytes[0] = 'f';
            bytes[1] = 'L';
            bytes[2] = 'a';
            bytes[3] = 'C';
        } else if (kind == "unit") {
            bytes[0] = 'U';
            bytes[1] = 'N';
            bytes[2] = 'I';
            bytes[3] = 'T';
        }
        return bytes;
    }

    std::vector<uint8_t> makeBytes(std::initializer_list<uint8_t> values)
    {
        return std::vector<uint8_t>(values);
    }

    bool registerNullCreatorWithResult(const std::string &extension)
    {
        return ExtractorFactory::registerExtractor(extension, ExtractorFactory::ExtractorCreator());
    }

    TEST_CASE("Factory create with invalid extension returns null")
    {
        class MinimalDataSource : public DataSource {
        public:
            sdk_utils::status_t initCheck() const override { return sdk_utils::OK; }
            ssize_t readAt(off64_t, void *, size_t) override { return 0; }
            sdk_utils::status_t getSize(off64_t *) override { return sdk_utils::OK; }
            uint32_t flags() override { return 0; }
            void close() override { }
            sdk_utils::status_t getAvailableSize(off64_t, off64_t *) override
            {
                return sdk_utils::OK;
            }
            std::string toString() override { return "Minimal"; }
            sdk_utils::status_t reconnectAtOffset(off64_t) override { return sdk_utils::OK; }
            std::string getUri() override { return "test://minimal"; }
        };

        MinimalDataSource source;
        auto extractor
            = ExtractorFactory::createExtractor(&source, ".invalid_extension_xyz", false);
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("Factory create with empty extension returns null")
    {
        class MinimalDataSource : public DataSource {
        public:
            sdk_utils::status_t initCheck() const override { return sdk_utils::OK; }
            ssize_t readAt(off64_t, void *, size_t) override { return 0; }
            sdk_utils::status_t getSize(off64_t *) override { return sdk_utils::OK; }
            uint32_t flags() override { return 0; }
            void close() override { }
            sdk_utils::status_t getAvailableSize(off64_t, off64_t *) override
            {
                return sdk_utils::OK;
            }
            std::string toString() override { return "Minimal"; }
            sdk_utils::status_t reconnectAtOffset(off64_t) override { return sdk_utils::OK; }
            std::string getUri() override { return "test://minimal"; }
        };

        MinimalDataSource source;
        auto extractor = ExtractorFactory::createExtractor(&source, "", false);
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("Factory create normalizes uppercase extension")
    {
        const std::string ext = ".factory_upper_unit_case";
        const bool registered = ExtractorFactory::registerExtractor(
            ext, [](DataSourceBase *source) -> ExtractorHelper * {
                return new TestOnlyExtractor(source);
            });
        REQUIRE(registered == true);

        TestFactoryDataSource source(makeMagicHeader("unit"));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".FACTORY_UPPER_UNIT_CASE", false));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Factory create adds missing dot to extension")
    {
        const std::string ext = ".factory_missing_dot_unit_case";
        const bool registered = ExtractorFactory::registerExtractor(
            ext, [](DataSourceBase *source) -> ExtractorHelper * {
                return new TestOnlyExtractor(source);
            });
        REQUIRE(registered == true);

        TestFactoryDataSource source(makeMagicHeader("unit"));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, "factory_missing_dot_unit_case", false));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Factory create can sniff magic when extension unsupported")
    {
        const std::string ext = ".factory_sniff_unit_case";
        const bool registered = ExtractorFactory::registerExtractor(
            ext,
            [](DataSourceBase *source) -> ExtractorHelper * {
                return new TestOnlyExtractor(source);
            },
            sniffUnitMagic);
        REQUIRE(registered == true);

        TestFactoryDataSource source(makeMagicHeader("unit"));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".unknown", true));
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);
    }

    TEST_CASE("Factory create with sniff enabled returns null on short header")
    {
        TestFactoryDataSource source(makeBytes({ 0x12, 0x34, 0x56 }));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".unknown", true));
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("Factory create with sniff enabled returns null on null source")
    {
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(nullptr, ".unknown", true));
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("Factory create with sniff enabled returns null when no sniffer matches")
    {
        TestFactoryDataSource source(makeBytes({ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                                 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F }));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".unknown", true));
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("Factory register duplicate extractor returns false")
    {
        const std::string ext = ".dup_unit_test_ext";
        const bool first      = ExtractorFactory::registerExtractor(
            ext, [](DataSourceBase *) -> ExtractorHelper      *{ return nullptr; });
        const bool second = ExtractorFactory::registerExtractor(
            ext, [](DataSourceBase *) -> ExtractorHelper * { return nullptr; });

        REQUIRE(first == true);
        REQUIRE(second == false);
    }

    TEST_CASE("Factory register extractor rejects empty extension")
    {
        const bool result = registerNullCreatorWithResult("");
        REQUIRE(result == false);
    }

    TEST_CASE("Factory register extractor rejects null creator")
    {
        const bool result = ExtractorFactory::registerExtractor(
            ".null_creator_unit_case", ExtractorFactory::ExtractorCreator());
        REQUIRE(result == false);
    }

    TEST_CASE("Factory two argument create delegates to non sniff path")
    {
        TestFactoryDataSource source(makeBytes({ 0x12, 0x34, 0x56, 0x78 }));
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, ".two_arg_missing_unit_case"));
        REQUIRE(extractor == nullptr);
    }

    TEST_CASE("ASF packetized payload from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("萨克斯机.asf"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("萨克斯机.asf", ".asf", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const std::vector<AudioBuffer::AudioBufferPtr> packets = extractor->getPacketizedMetaData();
        REQUIRE(packets.empty() == false);
        CHECK(packets.size() > 1);

        size_t total = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            REQUIRE(packets[i] != nullptr);
            CHECK(packets[i]->size() > 0);
            total += packets[i]->size();
        }

        REQUIRE(extractor->getMetaData() != nullptr);
        CHECK(total == extractor->getMetaData()->size());
    }
}

TEST_SUITE("AudioSpec comparison")
{
    TEST_CASE("Equal specs")
    {
        AudioSpec spec1;
        spec1.sampleRate = 44100;
        spec1.format     = AudioFormatS16;
        spec1.numChannel = 2;

        AudioSpec spec2;
        spec2.sampleRate = 44100;
        spec2.format     = AudioFormatS16;
        spec2.numChannel = 2;

        REQUIRE(spec1 == spec2);
    }

    TEST_CASE("Different sample rate")
    {
        AudioSpec spec1;
        spec1.sampleRate = 44100;
        spec1.format     = AudioFormatS16;
        spec1.numChannel = 2;

        AudioSpec spec2;
        spec2.sampleRate = 48000;
        spec2.format     = AudioFormatS16;
        spec2.numChannel = 2;

        REQUIRE_FALSE(spec1 == spec2);
    }

    TEST_CASE("Different format")
    {
        AudioSpec spec1;
        spec1.sampleRate = 44100;
        spec1.format     = AudioFormatS16;
        spec1.numChannel = 2;

        AudioSpec spec2;
        spec2.sampleRate = 44100;
        spec2.format     = AudioFormatS32;
        spec2.numChannel = 2;

        REQUIRE_FALSE(spec1 == spec2);
    }

    TEST_CASE("Different channel count")
    {
        AudioSpec spec1;
        spec1.sampleRate = 44100;
        spec1.format     = AudioFormatS16;
        spec1.numChannel = 2;

        AudioSpec spec2;
        spec2.sampleRate = 44100;
        spec2.format     = AudioFormatS16;
        spec2.numChannel = 1;

        REQUIRE_FALSE(spec1 == spec2);
    }
}

TEST_SUITE("ExtractorRealMedia")
{
    TEST_CASE("WAV audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music.wav", ".wav", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate == 16000);
        CHECK(spec.numChannel == 1);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("AAC audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("48000_fltp_1.aac"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("48000_fltp_1.aac", ".aac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("FLAC audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.flac"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.flac", ".flac", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("OGG audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("Ringtones-耳聆网.ogg"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("Ringtones-耳聆网.ogg", ".ogg", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("MP3 audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.128.mp3"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.128.mp3", ".mp3", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
    }

    TEST_CASE("M4A audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("摇滚乐_Freesound.m4a"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("摇滚乐_Freesound.m4a", ".m4a", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("AIFF audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("俱乐部音乐循环-Freesound.aiff"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("俱乐部音乐循环-Freesound.aiff", ".aiff", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("ASF audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("萨克斯机.asf"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("萨克斯机.asf", ".asf", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
        CHECK(spec.durationMs > 0);
        CHECK(spec.format != AudioFormatUnknown);
    }

    TEST_CASE("WMA audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("十七岁-陶喆.96.wma"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("十七岁-陶喆.96.wma", ".wma", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("AMR audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("小镇姑娘-陶喆.96.amr"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("小镇姑娘-陶喆.96.amr", ".amr", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("APE audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music-ape.ape"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music-ape.ape", ".ape", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("MKV audio spec from checked-in media")
    {
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music-mkv.mkv"));

        std::shared_ptr<FileSource> source;
        auto extractor = fixture.create("music-mkv.mkv", ".mkv", source);
        REQUIRE(source != nullptr);
        REQUIRE(extractor != nullptr);
        REQUIRE(extractor->initCheck() == sdk_utils::OK);

        const AudioSpec spec = extractor->getAudioSpec();
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }
}
