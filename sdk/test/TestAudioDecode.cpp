#include "AudioDecode.h"
#include "LogWrapper.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace std;

#define LOG_TAG "AudioDecodeTest"

class AudioDecodeTest : public AudioDecodeCallback {
private:
    AudioCodecID m_codecID;
    AudioDecode m_audioDecode;
    std::ifstream m_inFile;
    std::ofstream m_outFile;
    AudioSpec m_spec;
    uint32_t m_num;

public:
    AudioDecodeTest() = delete;
    AudioDecodeTest(std::string &file, std::string &outFile)
        : m_codecID(AUDIO_CODEC_ID_AAC)
        , m_audioDecode(m_codecID, this)
        , m_inFile(file, std::ios::binary)
        , m_outFile(outFile, std::ios::binary)
        , m_num(0)
    {
        LOG_INFO(LOG_TAG, "AudioDecodeTest construct");
    }
    ~AudioDecodeTest()
    {
        LOG_INFO(LOG_TAG, "AudioDecodeTest destruct");
        if (m_inFile.is_open()) {
            m_inFile.close();
        }
        if (m_outFile.is_open()) {
            m_outFile.close();
        }
    }
    void run()
    {
        int ret = 0;
        int inFileSize = 0;
        int inLen = 0;
        int outLen = 0;

        if (!m_inFile.is_open()) {
            LOG_ERROR(LOG_TAG, "open file failed");
            return;
        }
        if (!m_outFile.is_open()) {
            LOG_ERROR(LOG_TAG, "open file failed");
            return;
        }
        m_inFile.seekg(0, std::ios::end);
        inFileSize = m_inFile.tellg();
        m_inFile.seekg(0, std::ios::beg);
        LOG_INFO(LOG_TAG, "inFileSize: %d", inFileSize);
        char *inData = new char[inFileSize];
        m_inFile.read(inData, inFileSize);
        LOG_INFO(LOG_TAG, "inLen: %d", inLen);

        ret = m_audioDecode.decode(inData, inFileSize);
        if (ret == 0) {
            LOG_INFO(LOG_TAG, "decode success");
        } else if (ret < 0) {
            LOG_INFO(LOG_TAG, "decode failed");
        }
        delete[] inData;
        LOG_INFO(LOG_TAG, "%d %d %d", m_spec.sampleRate, m_spec.numChannel, m_spec.format);
    }
    void onAudioDecodeCallback(AudioDecodeSpec &out)
    {
        LOG_INFO(LOG_TAG, "callback: chan:%d samples:%d frame_size:%d lineSize:%d num:%lu",
            out.spec.numChannel, out.spec.samples, out.spec.bytesPerSample, out.lineSize[0],
            ++m_num);
#if 0
        m_outFile.write(
            (char *)out.lineData[0], out.spec.samples * out.spec.bytesPerSample);
#else
        for (int i = 0; i < out.spec.samples; i++) {
            for (int ch = 0; ch < out.spec.numChannel; ch++) {
                m_outFile.write((char *)out.lineData[ch] + out.spec.bytesPerSample * i,
                    out.spec.bytesPerSample);
            }
        }
#endif
    }
};

void TestCode()
{
    {
        std::string file = "./6-48000_fltp_1.aac";
        std::string outFile = "./6-48000_fltp_1.pcm";
        std::shared_ptr<AudioDecodeTest> audioDecodeTest
            = std::make_shared<AudioDecodeTest>(file, outFile);
        audioDecodeTest->run();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        std::string file = "./6-48000_fltp_2.aac";
        std::string outFile = "./6-48000_fltp_2.pcm";
        std::shared_ptr<AudioDecodeTest> audioDecodeTest
            = std::make_shared<AudioDecodeTest>(file, outFile);
        audioDecodeTest->run();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main()
{
    std::string rotateFileLog = "audio_decode_test";
    std::string directory = "./log";
    constexpr int k10MBInBytes = 10 * 1024 * 1024;
    constexpr int k20InCounts = 20;
    printf("LogWrapper::getInstanceInitialize\n");
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10MBInBytes, k20InCounts);
    LOG_INFO(LOG_TAG, "Log init success");

    TestCode();

    std::cout << "test end\n";
    return 0;
}