#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
}
using namespace std;

class AudioDecode {
private:
    /* data */
public:
    AudioDecode() = delete;
    AudioDecode(const string &fileName, const string &outFile)
    {
        
    }
    ~AudioDecode() { }

private:
    void decode()
    {
        // 1，打开音乐文件，调用av_open_input_file()
        // avformat_open_input(&m_formatCtx, fileName.c_str(), nullptr, nullptr);
        // 2，查找audio stream，调用av_find_stream_info()
        // avformat_find_stream_info(nullptr, nullptr)
        // 3，查找对应的decoder，调用avcodec_find_decoder()

        // 4，打开decoder，调用avcodec_open()

        // 5，读取一桢数据包，调用av_read_frame()

        // 6，解码数据包，调用avcodec_decode_audio3()

        // 7，将解码后的数据返回
    }
};

int main(int argc, char const *argv[])
{
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <audio file>" << endl;
        return -1;
    }

    string fileName(argv[1]);
    string outFile(fileName + ".pcm");
    cout << "fileName: " << fileName << endl;
    AudioDecode dec(fileName, outFile);
    return 0;
}
