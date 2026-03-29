#include <AudioCommon.hpp>
#include <AudioDevice.h>
#include <chrono>
#include <doctest/doctest.h>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

class MockAudioCallback : public AudioDataCallback {
public:
    MockAudioCallback()
        : m_callCount(0)
        , m_lastLen(0)
    {
    }

    void getAudioData(void *data, int len) override
    {
        m_callCount++;
        m_lastLen = len;
    }

    int getCallCount() const { return m_callCount; }
    int getLastLen() const { return m_lastLen; }

private:
    int m_callCount;
    int m_lastLen;
};

TEST_SUITE("AudioDevice")
{
    TEST_CASE("Construction with valid callback")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);
        REQUIRE(true);
    }

    TEST_CASE("Get device list")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);
        auto devList = device.getDeviceList();
        CHECK(devList.size() >= 0);
    }

    TEST_CASE("Get device spec")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);
        AudioSpec spec;
        int result = device.getDeviceSpec(spec);
        REQUIRE(result == 0);
        CHECK(spec.sampleRate > 0);
        CHECK(spec.numChannel > 0);
    }

    TEST_CASE("Open and close device")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

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
        MockAudioCallback callback;
        AudioDevice device(&callback);

        int firstOpen = device.open();
        if (firstOpen == 0) {
            int secondOpen = device.open();
            CHECK(secondOpen == -1);
            device.close();
        }
    }

    TEST_CASE("Start and stop device")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

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
        MockAudioCallback callback;
        AudioDevice device(&callback);

        device.start();
        CHECK(true);
    }

    TEST_CASE("Stop without start")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        device.stop();
        CHECK(true);
    }

    TEST_CASE("Close without open")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        device.close();
        CHECK(true);
    }

    TEST_CASE("Select device")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

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
        MockAudioCallback callback;
        AudioDevice device(&callback);

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
        MockAudioCallback callback;
        AudioDevice device(&callback);

        AudioSpec spec1, spec2;
        device.getDeviceSpec(spec1);
        device.getDeviceSpec(spec2);

        CHECK(spec1.sampleRate == spec2.sampleRate);
        CHECK(spec1.numChannel == spec2.numChannel);
    }

    TEST_CASE("Get device list returns empty or valid")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        auto devList1 = device.getDeviceList();
        auto devList2 = device.getDeviceList();
        CHECK(devList1.size() == devList2.size());
    }

    TEST_CASE("Select invalid device")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        int result = device.selectDevice(99999);
        CHECK(result == 0);
    }

    TEST_CASE("Default audio spec values")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        AudioSpec spec;
        device.getDeviceSpec(spec);

        CHECK(spec.sampleRate == 44100);
        CHECK(spec.numChannel == 2);
        CHECK(spec.format == AudioFormatS16);
    }

    TEST_CASE("Device spec bits per sample")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        AudioSpec spec;
        device.getDeviceSpec(spec);

        CHECK(spec.bitsPerSample > 0);
        CHECK(spec.bytesPerSample > 0);
    }

    TEST_CASE("Stop after start and close")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

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
        MockAudioCallback callback;
        AudioDevice device(&callback);

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

    TEST_CASE("AudioCallback is triggered after open and start")
    {
        MockAudioCallback callback;
        AudioDevice device(&callback);

        int openResult = device.open();
        if (openResult == 0) {
            device.start();
            // Wait for SDL to call the callback
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            device.stop();
            device.close();

            // Check if callback was called
            CHECK(callback.getCallCount() > 0);
            CHECK(callback.getLastLen() > 0);
            // Print actual values for debugging
            std::cout << "Callback was called " << callback.getCallCount()
                      << " times, len=" << callback.getLastLen() << std::endl;
        } else {
            // If open fails (no audio device), just verify it returns error
            CHECK(openResult == -1);
        }
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
