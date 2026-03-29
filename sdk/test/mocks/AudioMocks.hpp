#pragma once
#include "AudioDecode.h"
#include "AudioDevice.h"
#include <vector>

class MockAudioDecodeCallback : public AudioDecodeCallback {
public:
    void onAudioDecodeCallback(AudioDecodeSpec &out) override
    {
        m_callCount++;
        m_lastSpec = out.spec;
        if (out.lineData && out.lineSize) {
            for (int i = 0; i < out.spec.numChannel; i++) {
                m_lineSizes.push_back(out.lineSize[i]);
            }
        }
    }

    int m_callCount = 0;
    AudioSpec m_lastSpec;
    std::vector<int> m_lineSizes;
};

class MockAudioDataCallback : public AudioDataCallback {
public:
    void getAudioData(void *data, int len) override
    {
        m_callCount++;
        m_lastLen = len;
    }

    int m_callCount = 0;
    int m_lastLen   = 0;
};
