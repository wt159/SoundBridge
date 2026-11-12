#pragma once

#include "AudioDecode.h"
#include "DataSource.hpp"
#include "ExtractorHelper.hpp"
#include "NonCopyable.hpp"
#include <string>

struct AVFormatContext;
struct AVPacket;

// P2-1 HLS VOD 提取器（issue #144 / #143 契约 §3/§5）：FFmpeg 原生 URL 层直通。
//
// 装配（契约 §3 定稿）：不传自定义 pb —— avformat_open_input(&m_fmt, url, nullptr, &opts)，
// opts.protocol_whitelist = "http,tcp"（不含 file/https/crypto，网络源只许出网络）；
// interrupt_callback 复用 sbAbortInterruptCallback，opaque 挂数据源（UrlSource*，
// 同一 m_abortDecode 信号经它转发）；playlist 的 segment 相对 URI 由 hls demuxer
// 经 s->io_open 以同白名单重开。format 探测交给 FFmpeg（#EXTM3U 嗅探），
// 不显式 av_find_input_format。
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
};
