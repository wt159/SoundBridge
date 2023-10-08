#include "AudioDevice.h"
#include "LogWrapper.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>
using namespace std;

struct WAVHeader {
    char chunkID[4];
    uint32_t chunkSize;
    char format[4];
    char subchunk1ID[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char subchunk2ID[4];
    uint32_t subchunk2Size;
};

class AudioTest : public AudioDataCallback {
public:
    AudioTest(const char *fileName)
        : m_fileName(fileName)
        , m_file(fileName, std::ios::binary)
        , m_dev(this)
    {
        m_dev.getDeviceSpec(m_devSpec);
        if (init() != 0) {
            std::cout << "init failed" << std::endl;
        } else {
            std::cout << "init success" << std::endl;
        }
    }
    AudioTest(AudioTest &&) = default;
    AudioTest(const AudioTest &) = default;
    AudioTest &operator=(AudioTest &&) = default;
    AudioTest &operator=(const AudioTest &) = default;
    ~AudioTest()
    {
        m_dev.stop();
        m_dev.close();
        if (m_buffer) {
            delete[] m_buffer;
            m_buffer = nullptr;
        }
        m_file.close();
    }
    int play(){
        if (m_dev.open()) {
            std::cout << "open device failed" << std::endl;
            return -1;
        }
        if(m_dev.start()) {
            std::cout << "start device failed" << std::endl;
            return -2;
        }
        std::cout << "start device success" << std::endl;
        return 0;
    }

private:
    int init()
    {
        WAVHeader header;
        if (!m_file.is_open()) {
            std::cout << "open file failed" << std::endl;
            return -1;
        }
        m_file.seekg(0, std::ios::end);
        m_fileSize = m_file.tellg();
        m_file.seekg(0, std::ios::beg);

        m_file.read((char *)&header, sizeof(header));
        // 输出文件头信息
        std::cout << "Chunk ID: " << std::string(header.chunkID, 4) << std::endl;
        std::cout << "Chunk Size: " << header.chunkSize << " bytes" << std::endl;
        std::cout << "Format: " << std::string(header.format, 4) << std::endl;
        std::cout << "Subchunk1 ID: " << std::string(header.subchunk1ID, 4) << std::endl;
        std::cout << "Subchunk1 Size: " << header.subchunk1Size << " bytes" << std::endl;
        std::cout << "Audio Format: " << header.audioFormat << std::endl;
        std::cout << "Number of Channels: " << header.numChannels << std::endl;
        std::cout << "Sample Rate: " << header.sampleRate << " Hz" << std::endl;
        std::cout << "Byte Rate: " << header.byteRate << " bytes per second" << std::endl;
        std::cout << "Block Align: " << header.blockAlign << " bytes" << std::endl;
        std::cout << "Bits per Sample: " << header.bitsPerSample << " bits" << std::endl;
        std::cout << "Subchunk2 ID: " << std::string(header.subchunk2ID, 4) << std::endl;
        std::cout << "Subchunk2 Size: " << header.subchunk2Size << " bytes" << std::endl;
        m_spec.sampleRate = header.sampleRate;
        m_spec.numChannel = header.numChannels;
        m_spec.format = AudioFormatS16;
        m_spec.bitsPerSample = header.bitsPerSample;
        m_bufferPos = 0;
        m_bufferSize = m_fileSize - sizeof(header);
        std::cout << "\nfile size: " << m_fileSize << std::endl;
        std::cout << "buffer size: " << m_bufferSize << std::endl;
        m_buffer = new char[m_bufferSize];
        m_file.read(m_buffer, m_bufferSize);
        return 0;
    }
    
    void getAudioData(void *data, int len)
    {
        // std::cout << "onAudioData: " << len << std::endl;
        if(m_bufferPos >= m_bufferSize) {
            std::cout << "stop" << std::endl;
            m_dev.stop();
            return;
        }
        memset(data, 0, len);
        if (m_bufferPos + len > m_bufferSize) {
            std::cout << "buffer overflow" << std::endl;
            len = m_bufferSize - m_bufferPos;
        }
        memcpy(data, m_buffer + m_bufferPos, len);
        m_bufferPos += len;
    }

private:
    std::string m_fileName;
    std::ifstream m_file;
    int m_fileSize;
    char *m_buffer;
    int m_bufferSize;
    int m_bufferPos;
    AudioSpec m_spec;
    AudioSpec m_devSpec;
    AudioDevice m_dev;
};

int main()
{
    const char *fileName = "./music.wav";
    AudioTest test(fileName);
    test.play();
    std::cin.get();
    return 0;
}