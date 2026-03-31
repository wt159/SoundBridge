#include <AudioBuffer.h>
#include <AudioRingBuffer.h>
#include <ByteUtils.h>
#include <cstring>
#include <doctest/doctest.h>
#include <string>

TEST_SUITE("AudioRingBuffer")
{
    TEST_CASE("Default construction")
    {
        AudioRingBuffer rb;
        REQUIRE(rb.capacity() > 0);
        REQUIRE(rb.availableRead() == 0);
        REQUIRE(rb.availableWrite() > 0);
    }

    TEST_CASE("Custom capacity")
    {
        AudioRingBuffer rb(1024);
        REQUIRE(rb.capacity() == 1024);
        REQUIRE(rb.availableRead() == 0);
    }

    TEST_CASE("Basic write and read")
    {
        AudioRingBuffer rb(256);

        const char data[] = { 1, 2, 3, 4, 5 };
        size_t written    = rb.write(data, sizeof(data));
        REQUIRE(written == 5);

        REQUIRE(rb.availableRead() == 5);

        char read[5] = { 0 };
        size_t readn = rb.read(read, sizeof(read));
        REQUIRE(readn == 5);
        REQUIRE(std::memcmp(data, read, 5) == 0);
    }

    TEST_CASE("Empty buffer read")
    {
        AudioRingBuffer rb(256);
        REQUIRE(rb.availableRead() == 0);

        char read[10] = { 0 };
        size_t readn  = rb.read(read, 10);
        REQUIRE(readn == 0);
    }

    TEST_CASE("Partial read")
    {
        AudioRingBuffer rb(256);

        const char data[] = { 1, 2, 3, 4, 5 };
        rb.write(data, sizeof(data));

        char read[3] = { 0 };
        size_t readn = rb.read(read, 3);
        REQUIRE(readn == 3);
        REQUIRE(read[0] == 1);
        REQUIRE(read[1] == 2);
        REQUIRE(read[2] == 3);

        REQUIRE(rb.availableRead() == 2);
    }

    TEST_CASE("Reset")
    {
        AudioRingBuffer rb(256);
        const char data[100] = { 1 };
        rb.write(data, 100);

        REQUIRE(rb.availableRead() > 0);

        rb.reset();

        REQUIRE(rb.availableRead() == 0);
        REQUIRE(rb.availableWrite() == rb.capacity());
    }

    TEST_CASE("Write exceeds capacity")
    {
        AudioRingBuffer rb(16);

        const char data[32] = { 1 };
        size_t written      = rb.write(data, 32);
        REQUIRE(written == 16);
    }

    TEST_CASE("Write less than available")
    {
        AudioRingBuffer rb(256);

        REQUIRE(rb.availableWrite() == 256);

        const char data[10] = { 1 };
        size_t written      = rb.write(data, 10);
        REQUIRE(written == 10);
        REQUIRE(rb.availableWrite() == 246);
    }

    TEST_CASE("Wrap around scenario")
    {
        AudioRingBuffer rb(32);

        char data[32];
        for (int i = 0; i < 32; i++) {
            data[i] = static_cast<char>(i);
        }

        rb.write(data, 32);
        REQUIRE(rb.availableRead() == 32);

        char read[16];
        rb.read(read, 16);
        REQUIRE(rb.availableRead() == 16);

        rb.write(data, 16);
        REQUIRE(rb.availableRead() == 32);
    }
}

TEST_SUITE("ByteUtils")
{
    TEST_CASE("Endian detection")
    {
        bool isLittle  = sdk_utils::is_little_endian();
        uint16_t value = 0x1234;
        uint8_t *bytes = reinterpret_cast<uint8_t *>(&value);

        if (isLittle) {
            REQUIRE(bytes[0] == 0x34);
            REQUIRE(bytes[1] == 0x12);
        } else {
            REQUIRE(bytes[0] == 0x12);
            REQUIRE(bytes[1] == 0x34);
        }
    }

    TEST_CASE("U16/U32/U64 little endian read")
    {
        uint8_t data[8] = { 0 };
        data[0]         = 0x34;
        data[1]         = 0x12;

        uint16_t v16 = sdk_utils::U16LE_AT(data);
        REQUIRE(v16 > 0);

        data[0]      = 0x34;
        data[1]      = 0x12;
        data[2]      = 0x78;
        data[3]      = 0x56;
        uint32_t v32 = sdk_utils::U32LE_AT(data);
        REQUIRE(v32 > 0);

        data[4]      = 0xAB;
        data[5]      = 0xCD;
        data[6]      = 0xEF;
        data[7]      = 0x01;
        uint64_t v64 = sdk_utils::U64LE_AT(data);
        REQUIRE(v64 > 0);
    }

    TEST_CASE("U16/U32/U64 big endian read")
    {
        uint8_t data[8] = { 0 };
        data[0]         = 0x12;
        data[1]         = 0x34;
        uint16_t v16    = sdk_utils::U16_AT(data);
        REQUIRE(v16 > 0);

        data[2]      = 0x56;
        data[3]      = 0x78;
        uint32_t v32 = sdk_utils::U32_AT(data);
        REQUIRE(v32 > 0);

        data[4]      = 0x01;
        data[5]      = 0x23;
        data[6]      = 0x45;
        data[7]      = 0x67;
        uint64_t v64 = sdk_utils::U64_AT(data);
        REQUIRE(v64 > 0);
    }

    TEST_CASE("U16/U32/U64 big endian read")
    {
        uint8_t data[] = { 0x12, 0x34, 0x56, 0x78, 0x01, 0x23, 0x45, 0x67 };

        REQUIRE(sdk_utils::U16_AT(data) == 0x1234);
        REQUIRE(sdk_utils::U32_AT(data) == 0x12345678);
        REQUIRE(sdk_utils::U64_AT(data) == 0x1234567801234567ULL);
    }

    TEST_CASE("U16/U32/U64 big endian read")
    {
        uint8_t data[] = { 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78 };

        REQUIRE(sdk_utils::U16_AT(data) == 0x1234);
        REQUIRE(sdk_utils::U32_AT(data) == 0x12345678);
        REQUIRE(sdk_utils::U64_AT(data) == 0x1234567812345678ULL);
    }

    TEST_CASE("ntohl and htonl")
    {
        uint32_t value  = 0x12345678;
        uint32_t result = sdk_utils::htonl(value);

        if (sdk_utils::is_little_endian()) {
            REQUIRE(result == 0x78563412);
        } else {
            REQUIRE(result == value);
        }

        REQUIRE(sdk_utils::ntohl(result) == value);
    }

    TEST_CASE("ntoh64 and hton64")
    {
        uint64_t value  = 0x123456789ABCDEF0ULL;
        uint64_t result = sdk_utils::hton64(value);

        if (sdk_utils::is_little_endian()) {
            REQUIRE(result == 0xF0DEBC9A78563412ULL);
        } else {
            REQUIRE(result == value);
        }

        REQUIRE(sdk_utils::ntoh64(result) == value);
    }

    TEST_CASE("MakeFourCCString")
    {
        uint32_t fourcc = ('M') | ('P' << 8) | ('3' << 16) | (' ' << 24);
        char s[5]       = { 0 };
        sdk_utils::MakeFourCCString(fourcc, s);
        REQUIRE(s[4] == '\0');
    }
}

TEST_SUITE("AudioBuffer")
{
    TEST_CASE("Construction with size")
    {
        AudioBuffer buffer(1024);
        REQUIRE(buffer.size() == 1024);
        REQUIRE(buffer.data() != nullptr);
    }

    TEST_CASE("Construction with buffer")
    {
        char *data = new char[256]();
        AudioBuffer buffer(data, 256);
        REQUIRE(buffer.size() == 256);
        REQUIRE(buffer.data() == data);
    }

    TEST_CASE("setData and getData")
    {
        char original[256] = { 0 };
        for (int i = 0; i < 256; i++) {
            original[i] = static_cast<char>(i);
        }

        AudioBuffer buffer(256);
        buffer.setData(0, 256, original);

        char result[256] = { 0 };
        buffer.getData(0, 256, result);

        REQUIRE(std::memcmp(original, result, 256) == 0);
    }

    TEST_CASE("setData with offset")
    {
        char data[100] = { 1 };
        AudioBuffer buffer(100);

        buffer.setData(50, 10, data);

        char result[10] = { 0 };
        buffer.getData(50, 10, result);
        REQUIRE(result[0] == 1);
    }
}
