#include <AudioCommon.hpp>
#include <DataSource.hpp>
#include <ExtractorFactory.h>
#include <cstring>
#include <doctest/doctest.h>
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

TEST_SUITE("ExtractorFactory")
{
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
