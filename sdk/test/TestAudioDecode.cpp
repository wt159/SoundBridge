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

#pragma pack(push, 1)
struct ADTSHeader {
    uint16_t syncWord              : 12;
    uint8_t id                     : 1;
    uint8_t layer                  : 2;
    uint8_t protectionAbsent       : 1; //+0
    uint8_t profile                : 2;
    uint8_t samplingFrequencyIndex : 4;
    uint8_t privateBit             : 1; //+1
    uint8_t channelConfiguration   : 3;
    uint8_t originalCopy           : 1;
    uint8_t home                   : 1;
    uint8_t copyrightIDBit         : 1;
    uint8_t copyrightIDStart       : 1; //+2
    uint16_t frameLength           : 13;
    uint16_t bufferFullness        : 11;
    uint8_t numRawDataBlocks       : 2;
};
#pragma pack(pop)

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
        int ret        = 0;
        int inFileSize = 0;
        int inLen      = 0;

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
        LOG_INFO(LOG_TAG, "inLen: %d, adtsHeaderSize: %d", inLen, sizeof (ADTSHeader));
        ADTSHeader header;
        char *data = inData;
        header.syncWord = (data[0] << 4) | (data[1] >> 4);
        header.id = (data[1] >> 3) & 0x01;
        header.layer = (data[1] >> 1) & 0x03;
        header.protectionAbsent = data[1] & 0x01;
        header.profile = (data[2] >> 6) & 0x03;
        header.samplingFrequencyIndex = (data[2] >> 2) & 0x0F;
        header.privateBit = (data[2] >> 1) & 0x01;
        header.channelConfiguration = (((data[2] & 0x01) << 2) | ((data[3] & 0xc0) >> 6)) & 0x07;
        LOG_INFO(LOG_TAG, "data[2]: %x %x", data[2], data[3]);
        header.originalCopy = (data[3] >> 5) & 0x01;
        header.home = (data[3] >> 4) & 0x01;
        header.copyrightIDBit = (data[3] >> 3) & 0x01;
        header.copyrightIDStart = (data[3] >> 2) & 0x01;
        header.frameLength = ((data[3] & 0x03) << 11) | ((data[4] & 0xff) << 3) | ((data[5] & 0xe0) >> 5);
        header.bufferFullness = ((data[5] & 0x1F) << 6) | ((data[6] & 0xfc) >> 2);
        header.numRawDataBlocks = data[6] & 0x03;
        LOG_INFO(LOG_TAG, "syncWord: %x", header.syncWord);
        LOG_INFO(LOG_TAG, "id: %x", header.id);
        LOG_INFO(LOG_TAG, "layer: %x", header.layer);
        LOG_INFO(LOG_TAG, "protectionAbsent: %x", header.protectionAbsent);
        LOG_INFO(LOG_TAG, "profile: %x", header.profile);
        LOG_INFO(LOG_TAG, "samplingFrequencyIndex: %d", header.samplingFrequencyIndex);
        LOG_INFO(LOG_TAG, "privateBit: %x", header.privateBit);
        LOG_INFO(LOG_TAG, "channelConfiguration: %d", header.channelConfiguration);
        LOG_INFO(LOG_TAG, "originalCopy: %x", header.originalCopy);
        LOG_INFO(LOG_TAG, "home: %x", header.home);
        LOG_INFO(LOG_TAG, "frameLength: %d", header.frameLength);
        LOG_INFO(LOG_TAG, "bufferFullness: %d", header.bufferFullness);
        LOG_INFO(LOG_TAG, "numRawDataBlocks: %d", header.numRawDataBlocks);


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
        LOG_INFO(LOG_TAG,
                 "callback: chan:%d samples:%d frame_size:%d, fmtBit:%d lineSize:%d num:%lu",
                 out.spec.numChannel, out.spec.samples, out.spec.bytesPerSample,
                 out.spec.bitsPerSample, out.lineSize[0], ++m_num);
#if 0
        m_outFile.write((char *)out.lineData[0], out.lineSize[0]);
#else
        for (uint64_t i = 0; i < out.spec.samples; i++) {
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
        std::string file    = "./6-48000_fltp_1.aac";
        std::string outFile = "./7-48000_fltp_1.pcm";
        std::shared_ptr<AudioDecodeTest> audioDecodeTest
            = std::make_shared<AudioDecodeTest>(file, outFile);
        audioDecodeTest->run();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        std::string file    = "../../../music/WavinFlag.aac";
        std::string outFile = "./WavinFlag.pcm";
        std::shared_ptr<AudioDecodeTest> audioDecodeTest
            = std::make_shared<AudioDecodeTest>(file, outFile);
        audioDecodeTest->run();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main()
{
    std::string rotateFileLog  = "audio_decode_test";
    std::string directory      = "./log";
    constexpr int k10MBInBytes = 10 * 1024 * 1024;
    constexpr int k20InCounts  = 20;
    printf("LogWrapper::getInstanceInitialize\n");
    LogWrapper::getInstanceInitialize(directory, rotateFileLog, k10MBInBytes, k20InCounts);
    LOG_INFO(LOG_TAG, "Log init success");

    TestCode();

    std::cout << "test end\n";
    return 0;
}