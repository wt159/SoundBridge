#include "AudioResample.h"
#include "LogWrapper.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>

using namespace std;
#define LOG_TAG "AudioResampleTest"
class AudioResampleTest {
private:
    AudioResample m_resample;
    std::ifstream m_inFile;
    std::ofstream m_outFile;
    AudioSpec m_in;
    AudioSpec m_out;
public:
    AudioResampleTest(std::string &inFile, AudioSpec &in, std::string &outFile, AudioSpec &out)
        : m_resample()
        , m_inFile(inFile, std::ios::binary)
        , m_outFile(outFile, std::ios::binary)
        , m_in(in)
        , m_out(out)
    {
        m_resample.init(in, out);
    }
    void run()
    {
        int ret = 0;
        int inFileSize = 0;
        int outFileSize = 0;
        int inLen = 0;
        int outLen = 0;
        int inDataSize = m_in.bytes_frame_num * m_in.channels * m_in.bits_per_sample / 8;
        int outDataSize = m_out.bytes_frame_num * m_out.channels * m_out.bits_per_sample / 8;
        LOG_INFO(LOG_TAG, "inDataSize: %d, outDataSize: %d", inDataSize, outDataSize);
        if(!m_inFile.is_open() || !m_outFile.is_open()) {
            LOG_ERROR(LOG_TAG, "open file failed");
            return;
        }
        m_inFile.seekg(0, std::ios::end);
        inFileSize = m_inFile.tellg();
        LOG_INFO(LOG_TAG, "inFileSize: %d", inFileSize);
        m_inFile.seekg(0, std::ios::beg);
        char *inData = new char[inDataSize];
        char *outData = new char[outDataSize + 10];
        while(1) {
            inLen = m_inFile.read(inData, inDataSize).gcount();
            if(inLen <= 0) {
                LOG_ERROR(LOG_TAG, "read file failed");
                break;
            }
            // LOG_INFO(LOG_TAG, "inLen: %d", inLen);
            ret = m_resample.resample(inData, inLen, outData, &outLen);
            if(ret != 0) {
                LOG_ERROR(LOG_TAG, "resample failed, ret: %d", ret);
                break;
            }
            m_outFile.write(outData, outLen);
            outFileSize += outLen;
        }
        LOG_INFO(LOG_TAG, "outFileSize: %d", outFileSize);
        delete[] inData;
        delete[] outData;
    }
    ~AudioResampleTest() {
        if(m_inFile.is_open()) {
            m_inFile.close();
        }
        if(m_outFile.is_open()) {
            m_outFile.close();
        }
     }
};

void TestCode()
{
    std::string inFile = "./1-44100_s16le_2.pcm";
    AudioSpec in;
    in.sample_rate = 44100;
    in.channels = 2;
    in.format = AudioFormatS16;
    in.bytes_frame_num = 1024;
    in.bits_per_sample = 16;
    std::string outFile = "./2-48000_f32le_1.pcm";
    AudioSpec out;
    out.sample_rate = 48000;
    out.channels = 1;
    out.format = AudioFormatFloat32;
    out.bytes_frame_num = in.bytes_frame_num * out.sample_rate / in.sample_rate;
    out.bits_per_sample = 32;
    AudioResampleTest test(inFile, in, outFile, out);
    test.run();
}

int main()
{
    std::string rotateFileLog = "audio_resample_test";
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