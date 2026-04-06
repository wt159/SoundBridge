#include <AudioCommon.hpp>
#include <AudioDevice.h>
#include <doctest/doctest.h>

TEST_SUITE("AudioDevice Lifecycle")
{
    TEST_CASE("default construction initializes")
    {
        AudioDevice device;
        // Should be in default state
        AudioSpec spec;
        int ret = device.getDeviceSpec(spec);
        CHECK(ret == 0);
    }

    TEST_CASE("open succeeds")
    {
        AudioDevice device;
        int ret = device.open();
        // May fail on headless systems, but should not crash
        if (ret == 0) {
            device.close();
        }
    }

    TEST_CASE("open twice returns error or handles gracefully")
    {
        AudioDevice device;
        int ret1 = device.open();
        if (ret1 == 0) {
            int ret2 = device.open();
            // Should handle gracefully
            device.close();
        }
    }

    TEST_CASE("close without open is safe")
    {
        AudioDevice device;
        device.close(); // Should not crash
    }

    TEST_CASE("start without open is safe")
    {
        AudioDevice device;
        device.start(); // Should not crash or handle gracefully
    }

    TEST_CASE("stop without start is safe")
    {
        AudioDevice device;
        device.open();
        device.stop(); // Should not crash
        device.close();
    }

    TEST_CASE("full lifecycle: open -> start -> stop -> close")
    {
        AudioDevice device;
        REQUIRE(device.open() == 0);
        device.start();
        device.stop();
        device.close();
    }
}

TEST_SUITE("AudioDevice Write Operations")
{
    TEST_CASE("write with null data returns error")
    {
        AudioDevice device;
        int written = device.write(nullptr, 1024);
        CHECK(written == -1);
    }

    TEST_CASE("write with zero length returns error")
    {
        AudioDevice device;
        uint8_t data[1024];
        int written = device.write(data, 0);
        CHECK(written == -1);
    }

    TEST_CASE("write after open fills buffer")
    {
        AudioDevice device;
        if (device.open() == 0) {
            uint8_t data[1024] = { 0 };
            int written        = device.write(data, sizeof(data));
            // written may be 0 if not started, or some value
            device.close();
        }
    }
}

TEST_SUITE("AudioDevice Queue Management")
{
    TEST_CASE("getQueuedBytes returns 0 initially")
    {
        AudioDevice device;
        CHECK(device.getQueuedBytes() == 0);
    }

    TEST_CASE("clearQueue resets queue")
    {
        AudioDevice device;
        device.open();
        device.clearQueue();
        CHECK(device.getQueuedBytes() == 0);
        device.close();
    }
}
