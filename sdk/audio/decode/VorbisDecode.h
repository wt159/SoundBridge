#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include <atomic>
#include <vector>
#include <vorbis/vorbisfile.h>

// decodeOne() 的返回值：0 = EOS，1 = 解出一段，小于 0 = 失败。
// `OV_HOLE` 不是失败——它是「这一页坏了，后面还有」（规格 4.3 / 条目 12），
// 单列一个正值让调用方把「跳页」和「出错」分清。
constexpr int kVorbisHoleSkipped = 2;

class VorbisDecode {
private:
    AudioDecodeCallback *m_callback;
    AudioDecodeSpec m_decSpec;
    std::atomic<bool> *m_abortFlag;
    const uint8_t *m_data;
    size_t m_size;
    size_t m_pos;
    DataSourceBase *m_dataSourcePtr;
    OggVorbis_File m_vf;
    bool m_vfOpened;
    std::vector<uint8_t> m_interleavedBuf;
    // 连续空洞计数：全是垃圾时不能无限 ov_read 下去（与 ffmpeg 侧 32 帧护栏同义）
    int m_consecutiveHoles;
    int m_holeCount;
    // 可 seek 打开时 libvorbisfile 会用 ov_raw_seek 预热；首个音频页如果是空洞，
    // 它把 `_get_next_page()<0` 当成文件尾，一个包都不缓冲就「成功」返回（条目 13）。
    // 这种情况下退化成不可 seek 的流式打开，把整段解出来。
    bool m_streamingFallbackTried;
    bool m_streamingMode;
    // 是否已经解出过音频：用来区分「正常到结尾」与「预热阶段一个包都没进」。
    bool m_producedAudio;

    static size_t readCallback(void *ptr, size_t size, size_t nmemb, void *datasource);
    static int seekCallback(void *datasource, ogg_int64_t offset, int whence);
    static int closeCallback(void *datasource);
    static long tellCallback(void *datasource);

    void clearDecodeSpec();
    bool ensureDecodeSpec(int channels, int sampleRate, uint64_t samples);
    bool open(bool seekable);
    bool reopenAsStreaming();

public:
    VorbisDecode(AudioDecodeCallback *callback);
    ~VorbisDecode();
    bool initVF(const char *data, size_t size);
    bool initVFFromSource(DataSourceBase *src, off64_t size);
    int decodeOne();
    bool seekToMs(uint64_t targetMs);
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);
    void setAbortFlag(std::atomic<bool> *flag);

protected:
};
