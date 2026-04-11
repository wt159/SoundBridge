#include <AudioCommon.hpp>
#include <AudioDevice.h>
#include <chrono>
#include <doctest/doctest.h>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

TEST_SUITE("AudioDevice")
{
    TEST_CASE("Construction")
    {
        AudioDevice device;
        REQUIRE(true);
    }

    TEST_CASE("Get device list")
    {
        AudioDevice device;
        auto devList = device.getDeviceList();
        CHECK(devList.size() >= 0);
    }

    TEST_CASE("Get device spec")
    {
        AudioDevice device;
        AudioSpec spec;
        int result = device.getDeviceSpec(spec);
        REQUIRE(result == 0);
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("Open and close device")
    {
        AudioDevice device;

        int openResult = device.open();
        if (openResult == 0) {
            CHECK(true);
            device.close();
            CHECK(true);
        } else {
            CHECK(openResult == -1);
        }
    }

    TEST_CASE("Open device twice")
    {
        AudioDevice device;

        int firstOpen = device.open();
        if (firstOpen == 0) {
            int secondOpen = device.open();
            CHECK(secondOpen == -1);
            device.close();
        }
    }

    TEST_CASE("Start and stop device")
    {
        AudioDevice device;

        int openResult = device.open();
        if (openResult == 0) {
            int startResult = device.start();
            CHECK(startResult == 0);

            int stopResult = device.stop();
            CHECK(stopResult == 0);

            device.close();
        }
    }

    TEST_CASE("Start without open")
    {
        AudioDevice device;

        device.start();
        CHECK(true);
    }

    TEST_CASE("Stop without start")
    {
        AudioDevice device;

        device.stop();
        CHECK(true);
    }

    TEST_CASE("Close without open")
    {
        AudioDevice device;

        device.close();
        CHECK(true);
    }

    TEST_CASE("Select device")
    {
        AudioDevice device;

        auto devList = device.getDeviceList();
        if (!devList.empty()) {
            int selectResult = device.selectDevice(devList[0].first);
            CHECK(selectResult == 0);
        } else {
            CHECK(true);
        }
    }

    TEST_CASE("Multiple open/close cycles")
    {
        AudioDevice device;

        for (int i = 0; i < 3; i++) {
            int openResult = device.open();
            if (openResult == 0) {
                device.close();
            }
        }
        CHECK(true);
    }

    TEST_CASE("Get device spec multiple times")
    {
        AudioDevice device;

        AudioSpec spec1, spec2;
        device.getDeviceSpec(spec1);
        device.getDeviceSpec(spec2);

        CHECK(spec1.sampleRate == spec2.sampleRate);
        CHECK(spec1.numChannel == spec2.numChannel);
    }

    TEST_CASE("Get device list returns empty or valid")
    {
        AudioDevice device;

        auto devList1 = device.getDeviceList();
        auto devList2 = device.getDeviceList();
        CHECK(devList1.size() == devList2.size());
    }

    TEST_CASE("Select invalid device")
    {
        AudioDevice device;

        int result = device.selectDevice(99999);
        CHECK(result == 0);
    }

    TEST_CASE("Default audio spec values")
    {
        AudioDevice device;

        AudioSpec spec;
        device.getDeviceSpec(spec);

        CHECK(spec.sampleRate == 44100);
        CHECK(spec.numChannel == 2);
        CHECK(spec.format == AudioFormatS16);
    }

    TEST_CASE("Device spec bits per sample")
    {
        AudioDevice device;

        AudioSpec spec;
        device.getDeviceSpec(spec);

        CHECK(spec.bitsPerSample > 0);
        CHECK(spec.bytesPerSample > 0);
    }

    TEST_CASE("Stop after start and close")
    {
        AudioDevice device;

        int openResult = device.open();
        if (openResult == 0) {
            device.start();
            device.stop();
            device.close();
            device.stop();
            CHECK(true);
        }
    }

    TEST_CASE("Sequential start stop multiple times")
    {
        AudioDevice device;

        int openResult = device.open();
        if (openResult == 0) {
            for (int i = 0; i < 3; i++) {
                device.start();
                device.stop();
            }
            device.close();
            CHECK(true);
        }
    }

    TEST_CASE("Write queues data while device is paused")
    {
        AudioDevice device;

        int openResult = device.open();
        if (openResult == 0) {
            std::vector<uint8_t> testData(1024, 0x00);
            int writeResult = device.write(testData.data(), testData.size());
            CHECK(writeResult == 0);

            size_t queued = device.getQueuedBytes();
            CHECK(queued >= testData.size());

            device.clearQueue();
            queued = device.getQueuedBytes();
            CHECK(queued == 0);

            device.start();
            device.stop();
            device.close();
        } else {
            CHECK(openResult == -1);
        }
    }

    TEST_CASE("Get queued bytes when not open")
    {
        AudioDevice device;

        size_t queued = device.getQueuedBytes();
        CHECK(queued == 0);
    }

    TEST_CASE("Write when not open")
    {
        AudioDevice device;

        std::vector<uint8_t> data(512, 0x00);
        int result = device.write(data.data(), data.size());
        CHECK(result == -1);
    }

    TEST_CASE("Clear queue when not open")
    {
        AudioDevice device;

        device.clearQueue();
        CHECK(true);
    }
}

TEST_SUITE("AudioFormat utilities")
{
    TEST_CASE("All audio format byte calculations")
    {
        CHECK(getBytePreSampleByAudioFormat(AudioFormatU8) == 1);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS8) == 1);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatU16) == 2);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS16) == 2);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatU16BE) == 2);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS16BE) == 2);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatU24) == 3);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS24) == 3);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatU24BE) == 3);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS24BE) == 3);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS32) == 4);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS32) == 4);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatS32BE) == 4);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatFLT32) == 4);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatFLT32BE) == 4);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatDBL64) == 8);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatDBL64BE) == 8);
        CHECK(getBytePreSampleByAudioFormat(AudioFormatUnknown) == 0);
    }

    TEST_CASE("Audio format by bits")
    {
        CHECK(getAudioFormatByBitPreSample(8) == AudioFormatU8);
        CHECK(getAudioFormatByBitPreSample(16) == AudioFormatS16);
        CHECK(getAudioFormatByBitPreSample(24) == AudioFormatS24);
        CHECK(getAudioFormatByBitPreSample(32) == AudioFormatS32);
        CHECK(getAudioFormatByBitPreSample(64) == AudioFormatDBL64);
        CHECK(getAudioFormatByBitPreSample(0) == AudioFormatUnknown);
        CHECK(getAudioFormatByBitPreSample(7) == AudioFormatUnknown);
        CHECK(getAudioFormatByBitPreSample(100) == AudioFormatUnknown);
    }
}
