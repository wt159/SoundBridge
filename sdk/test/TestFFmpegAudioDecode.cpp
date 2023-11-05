#include <algorithm>
#include <fstream>
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
    AVFormatContext *m_formatCtx;
    AVCodecContext *m_codecCtx;
    AVCodec *m_codec;
    AVPacket *m_pkt;
    AVFrame *m_frame;
    std::ofstream m_outFile;
    int m_initCheck;
    int m_audioStream;
    char m_error[AV_ERROR_MAX_STRING_SIZE];

public:
    int initCheck() const { return m_initCheck; }
    char *getAVErrorString(int errnum)
    {
        av_strerror(errnum, m_error, sizeof(m_error));
        return m_error;
    }
    AudioDecode() = delete;
    AudioDecode(const string &fileName, const string &outFile)
        : m_formatCtx(nullptr)
        , m_codecCtx(nullptr)
        , m_codec(nullptr)
        , m_pkt(nullptr)
        , m_frame(nullptr)
        , m_outFile(outFile, std::ios::binary)
        , m_initCheck(-1)
        , m_audioStream(-1)
    {
        int ret = -1;
        av_register_all();
        avformat_network_init();
        av_log_set_level(AV_LOG_TRACE);
        av_log_set_callback([](void *avcl, int level, const char *fmt, va_list vl) {
            char line[1024];
            vsnprintf(line, sizeof(line), fmt, vl);
            switch (level) {
            case AV_LOG_INFO:
                cout << "INFO: " << line << endl;
                break;
            case AV_LOG_WARNING:
                cout << "WARNING: " << line << endl;
                break;
            case AV_LOG_ERROR:
                cout << "ERROR: " << line << endl;
                break;
            case AV_LOG_FATAL:
                cout << "FATAL: " << line << endl;
                break;
            default:
                break;
            }
        });
        m_formatCtx = avformat_alloc_context();

        if (avformat_open_input(&m_formatCtx, fileName.c_str(), nullptr, nullptr) != 0) {
            cout << "avformat_open_input failed" << endl;
            return;
        }

        if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
            cout << "avformat_find_stream_info failed" << endl;
            return;
        }

        av_dump_format(m_formatCtx, 0, fileName.c_str(), 0);
        cout << " av dump format end" << endl;

        for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
            if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                m_audioStream = i;
                break;
            }
        }
        if (m_audioStream == -1) {
            cout << "Don`t find a audio stream" << endl;
            return;
        }

        m_codecCtx = avcodec_alloc_context3(nullptr);
        if (m_codecCtx == nullptr) {
            cout << "avcodec_alloc_context3 failed" << endl;
            return;
        }

        ret = avcodec_parameters_to_context(m_codecCtx,
                                            m_formatCtx->streams[m_audioStream]->codecpar);
        if (ret < 0) {
            cout << "avcodec_parameters_to_context failed" << endl;
            return;
        }
        cout << "channels: " << m_codecCtx->channels << endl;
        cout << "sample_fmt: " << m_codecCtx->sample_fmt << endl;
        cout << "sample_rate: " << m_codecCtx->sample_rate << endl;
        cout << "frame_size: " << m_codecCtx->frame_size << endl;
        cout << "codec_id: " << m_codecCtx->codec_id << endl;

        m_codec = avcodec_find_decoder(m_codecCtx->codec_id);
        if (m_codec == nullptr) {
            cout << "avcodec_find_decoder failed" << endl;
            return;
        }

        if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) {
            cout << "avcodec_open2 failed" << endl;
            return;
        }

        m_pkt   = av_packet_alloc();
        m_frame = av_frame_alloc();

        m_initCheck = 0;
    }
    ~AudioDecode()
    {
        if (m_pkt) {
            av_packet_free(&m_pkt);
        }
        if (m_frame) {
            av_frame_free(&m_frame);
        }
        if (m_codecCtx) {
            avcodec_close(m_codecCtx);
        }
        if (m_formatCtx) {
            avformat_close_input(&m_formatCtx);
        }
        if (m_outFile.is_open()) {
            m_outFile.close();
        }
    }

public:
    int decode()
    {
        // 1，打开音乐文件，调用av_open_input_file()
        // 2，查找audio stream，调用av_find_stream_info()
        // 3，查找对应的decoder，调用avcodec_find_decoder()
        // 4，打开decoder，调用avcodec_open()
        // 5，读取一桢数据包，调用av_read_frame()
        // 6，解码数据包，调用avcodec_decode_audio3()
        // 7，将解码后的数据返回
        cout << "decode start" << endl;
        int ret = 0, got_frame = 0;
        size_t num = 0, gotFrameNum = 0;
        while (1) {
            ret = av_read_frame(m_formatCtx, m_pkt);
            /* if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return;
            }
            else  */
            if (ret <= 0) {
                cout << "av_read_frame failed, ret:" << ret << " " << getAVErrorString(ret) << endl;
                ret = -1;
                break;
            }
            if (m_pkt->size <= 0)
                continue;
            ret = avcodec_send_packet(m_codecCtx, m_pkt);
            if (ret <= 0) {
                cout << "avcodec_send_packet failed, ret:" << ret << " " << getAVErrorString(ret)
                     << endl;
                ret = -1;
                break;
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret < 0) {
                    // LOG_ERROR(LOG_TAG, "avcodec_receive_frame failed %d:%s", ret,
                    // getAVErrorString(ret));
                    cout << "avcodec_receive_frame failed, ret:" << ret << " "
                         << getAVErrorString(ret) << endl;
                    break;
                }
                int bytesPerSample = av_get_bytes_per_sample(m_codecCtx->sample_fmt);
                // m_outFile.write((char *)m_frame->data[0], m_frame->linesize[0]);
                for (int i = 0; i < m_frame->nb_samples; i++) {
                    for (int ch = 0; ch < m_codecCtx->channels; ch++) {
                        m_outFile.write((char *)m_frame->data[ch] + bytesPerSample * i,
                                        bytesPerSample);
                    }
                }
            }
            num++;
            // if (got_frame > 0) {
            //     gotFrameNum++;
            //     int bytesPerSample = av_get_bytes_per_sample(m_codecCtx->sample_fmt);
            //     m_outFile.write((char *)m_frame->data[0], m_frame->linesize[0]);
            //     for (int i = 0; i < m_frame->nb_samples; i++) {
            //         for (int ch = 0; ch < m_codecCtx->channels; ch++) {
            //             m_outFile.write((char *)m_frame->data[ch] + bytesPerSample * i,
            //                             bytesPerSample);
            //         }
            //     }
            // }
        }
        cout << "decode end, num:" << num << " gotFrameNum:" << gotFrameNum << endl;
        return ret;
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
    if (dec.initCheck() != 0) {
        cout << "AudioDecode init failed" << endl;
        return -1;
    }
    if (dec.decode() != 0) {
        cout << "AudioDecode decode failed" << endl;
        return -2;
    }
    return 0;
}
