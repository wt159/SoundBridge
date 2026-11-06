#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "ErrorUtils.h"
#include <memory>
#include <vector>

struct AudioCodecConfig {
    int sampleRate;
    int channels;
    int bitRate;
    int blockAlign;
    int bitsPerSample;
    AudioBuffer::AudioBufferPtr extraData;

    AudioCodecConfig()
        : sampleRate(0)
        , channels(0)
        , bitRate(0)
        , blockAlign(0)
        , bitsPerSample(0)
        , extraData(nullptr)
    {
    }
};

struct AudioDecodeSpec {
    AudioSpec spec;
    uint8_t **lineData;
    int *lineSize;

    AudioDecodeSpec()
        : lineData(nullptr)
        , lineSize(nullptr)
    {
    }
};

class AudioDecodeCallback {
public:
    virtual ~AudioDecodeCallback() { }
    virtual void onAudioDecodeCallback(AudioDecodeSpec &out) = 0;
    // 「这一包解不出，已经跳过」的通知（规格 4.3 条目 11）。按发生顺序回调，
    // 所以调用方能分清「跳帧之后还有数据」（坏帧）与「跳完就到结尾」（截断）。
    virtual void onAudioDecodeSkip() { }
};

class AudioDecode {
public:
    AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback);
    AudioDecode(AudioCodecID codec, AudioDecodeCallback *callback, const AudioCodecConfig &config);
    ~AudioDecode();
    sdk_utils::status_t initCheck();
    int decode(const char *data, ssize_t size);
    int decodePackets(const std::vector<AudioBuffer::AudioBufferPtr> &packets);
    // —— 增量供给面（#62 / ADR 0003）——
    // 解码线程逐包喂入（包边界由包工厂给），不 flush；序列末尾调用 flushDecode()。
    // 与 decodePackets() 的区别：不拼整块、不复用 ffmpeg parser（parser 会把坏数据段
    // 吞进相邻帧，坏帧就再也报不出来），整个播放期只建一次编解码器。
    // 这不是新的同步解码入口（#18）：没有调用方循环，驱动仍来自解码线程的 pull。
    int decodePacket(const AudioBuffer::AudioBufferPtr &packet);
    int flushDecode();
    // 单包解不出而被跳过的包数（规格 4.3 条目 11）。只进日志与 detail，不做机器可读的
    // 错误字段（规格 F3）；测试用它验证「连续失败到阈值就放弃、不扫完整段垃圾」。
    int skippedPacketCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
