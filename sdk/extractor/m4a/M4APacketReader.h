#pragma once

#include "AudioBuffer.h"
#include "DataSource.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

// m4a 的 demux 面（票面 #64 / ADR 0003「包为唯一层间单元 / 读过 ≠ 常驻」）。
//
// 包边界只有解复用器知道：变长帧 / 块的字节长度无法从采样数反推。播放期重开一次
// demux、按 offset 顺序读包——每拉一包，边界由 demux 现场解析得到，不保留逐包时间轴、
// 不建 seek 表、不缓存已交出的包。全局索引在尾部的容器（m4a 的 moov、asf 的索引对象）
// 由 ffmpeg demux 内部按需读取，自仓不保留任何随包数增长的结构。
//
// 常驻状态是 O(1)：一个 AVIO 缓冲（判据公式里的独立项）+ 当前包副本（交出后归解码侧）。
// init 期的只读扫描也用它：读完整份文件只为累加标称时长，之后把 demux 整个关掉。
struct M4AStreamInfo {
    M4AStreamInfo();
    int codecId;
    int sampleRate;
    int channels;
    int bitsPerCodedSample;
    int bitsPerRawSample;
    int bitRate;
    int blockAlign;
    // 包时长刻度 → 毫秒（av_q2d(time_base) * 1000）。
    double timeBaseMs;
    // codec 的 extradata：几十字节的定长摘要，不是音频负载。
    std::vector<uint8_t> extradata;
};

class M4APacketReader {
public:
    M4APacketReader(DataSourceBase *source, int64_t fileSize);
    ~M4APacketReader();

    // demux 句柄在播放期由解码线程第一次拉包时创建（规格 4.3 所有权清单）。
    bool open();
    void close();
    bool isOpen() const { return m_fmt != nullptr; }

    // open() 之后读出音频流的定长事实（spec / 标称时长的换算刻度 / extradata）。
    bool streamInfo(M4AStreamInfo &info) const;

    // 拉下一包；返回 false 表示序列结束（含坏数据导致的提前结束，交 EOS 汇总兜底）。
    bool nextPacket(AudioBuffer::AudioBufferPtr &packet);
    // 上一包的时长（stream time_base 刻度）：只给 init 的只读扫描累加标称时长用。
    int lastPacketDuration() const { return m_lastPacketDuration; }
    // 容器声明的时长（微秒，0 表示未知）：只是兜底来源，默认不用它（见 init 的时长归属注释）。
    int64_t declaredDurationUs() const;

    // 定位到文件偏移：顺序重放 seek 只用 0（回到流头重新迭代）。
    bool seekTo(int64_t offset);
    // 下一次读块的位置（文件偏移），用于分辨「包序列在推进」与「原地打转」。
    int64_t position() const;

    // 现场重算包边界的 CPU 成本（独立指标，不设阈值）。
    uint64_t boundaryMicros() const { return m_boundaryMicros.load(); }

private:
    struct AvioData {
        DataSourceBase *source;
        int64_t pos;
        int64_t size;
    };

    static int avioRead(void *opaque, uint8_t *buf, int buf_size);
    static int64_t avioSeek(void *opaque, int64_t offset, int whence);

    // AAC 的 ADTS 包装（m4a 的容器差异）：mov demux 交出的是裸 AAC 帧，解码侧的
    // aac parser 与量测仪器都靠 ADTS 头切帧，所以按 extradata 里的 ASC 还原 7 字节头。
    // ALAC 不走这里（无 parser，边界由提取器直接给）。
    static int sampleRateToIndex(int sampleRate);
    static bool parseAudioSpecificConfig(const uint8_t *data, int size, int &audioObjectType,
                                        int &sampleRateIndex, int &channelConfig);
    static void buildAdtsHeader(uint8_t *out, int profile, int sampleRateIndex,
                                int channelConfig, int frameLen);
    void prepareAdts();

    AvioData m_ioData;
    AVFormatContext *m_fmt;
    AVIOContext *m_avio;
    AVPacket *m_pkt;
    int m_audioIndex;
    int m_lastPacketDuration;
    std::atomic<uint64_t> m_boundaryMicros;
    bool m_useAdts;
    int m_adtsProfile;
    int m_adtsSampleRateIndex;
    int m_adtsChannelConfig;
};
