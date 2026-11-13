#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <string>

struct AVFormatContext;
struct AVPacket;

// P2-1 HLS VOD 提取器（issue #144 / #143 契约 §3/§5）：FFmpeg 原生 URL 层直通。
// P3 起职能扩展为「直通提取器」（doc/live-source-spec.md §2）：avformat_open_input
// 自动探测 demuxer，同时承载 HLS LIVE 与 ICY（#152 D19），诊断旁路对两者同一覆盖。
//
// 装配（契约 §3 定稿）：不传自定义 pb —— avformat_open_input(&m_fmt, url, nullptr, &opts)，
// opts.protocol_whitelist = "http,tcp"（不含 file/https/crypto，网络源只许出网络）；
// opts.rw_timeout = 3000000（#155 D15：播放主路径唯一墙钟，经 av_opt_copy 下传 tcp、
// 也在 hls 7 项抄写里覆盖 segment；只设 rw_timeout 不设 timeout——后者经 tcp.c:139-141
// 静默覆盖前者且不在 7 项抄写内）；interrupt_callback 复用 sbAbortInterruptCallback，
// opaque 挂数据源（UrlSource*，同一 m_abortDecode 信号经它转发）；playlist 的 segment
// 相对 URI 由 hls demuxer 经 s->io_open 以同白名单重开。format 探测交给 FFmpeg
// （#EXTM3U 嗅探），不显式 av_find_input_format。
//
// init 只做 open + streamInfo：VOD 由 hls demuxer 提供 duration（declaredDurationUs
// 口径与 M4APacketReader::declaredDurationUs 一致，即 AVFormatContext::duration），
// 不做逐包全量扫描（直通路径的负载远端拉取，扫描即整段下载）。
// 包仍按 ADR 0003 逐包 pull（包是唯一层间单元），总数事先未知。
class HlsExtractor : public ExtractorHelper, public NonCopyable {
public:
    explicit HlsExtractor(DataSourceBase *source);
    virtual ~HlsExtractor();

    virtual sdk_utils::status_t initCheck() { return m_initCheck; }
    virtual AudioSpec getAudioSpec() { return m_audioSpec; }
    virtual AudioCodecID getAudioCodecID() { return m_audioCodecID; }
    virtual AudioBuffer::AudioBufferPtr getCodecExtraData() { return m_codecExtraData; }
    virtual DataSourceBase *getDataSource() { return m_source; }

    // 包工厂的单包面（ADR 0003）：总数事先未知，顺序拉取；index==0 回到流头重放。
    virtual size_t packetCount() { return ExtractorHelper::kUnknownPacketCount; }
    virtual AudioBuffer::AudioBufferPtr packetAt(size_t index);

    // 分配记账：demux 负载不常驻（交出去的包归解码侧记账），AVIO 缓冲是固定容量项，
    // 口径与 M4AExtractor 一致不计入。
    virtual size_t heldPayloadBytes() const { return 0; }

    // P3 诊断旁路（#151 / ADR 0006）：最近一次 av_read_frame 负返回，消费一次失效。
    // 只读状态查询，不是第二个层间单元；成功读包即清零（三条生命周期不变量见 .cpp）。
    virtual bool consumeLastReadError(int *outError);

    // P3 无状态重开原语（#149 / ADR 0007）：closeFmt + m_consumed=0 + open。
    // 重开从新滑动窗口头顺序迭代（不 seek——LIVE 上 av_seek_frame 必失败且错误被
    // hls 洗成裸 -1）；不持预算、不 sleep、不重试。返回 open 是否 OK。
    virtual bool reopenConnection();

    virtual bool supportsLiveReconnect() const { return true; }

private:
    sdk_utils::status_t open();
    void closeFmt();

    DataSourceBase *m_source;
    std::string m_url;
    AVFormatContext *m_fmt;
    AVPacket *m_pkt;
    int m_audioIndex;
    // 已顺序交出的包数（顺序拉取契约的游标，O(1) 常驻）。
    size_t m_consumed;
    sdk_utils::status_t m_initCheck;
    AudioSpec m_audioSpec;
    AudioCodecID m_audioCodecID;
    AudioBuffer::AudioBufferPtr m_codecExtraData;
    int64_t m_declaredDurationUs;
    // P3 诊断旁路状态（#151 D13）：m_diagValid=false 即「无失败」。
    // 提取器与 PacketCursor 同在解码线程，无需原子 / release-acquire。
    int m_lastReadError;
    bool m_diagValid;
};
