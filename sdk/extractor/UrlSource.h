#pragma once

#include "DataSource.hpp"
#include "NonCopyable.hpp"
#include <string>
#include <vector>

// P2-1 HLS VOD 直通数据源（issue #144 / #143 契约 §4）：持 URL、不持常驻 socket。
// FFmpeg 原生 URL 层自取数据，本类不供数：readAt() 一律失败、size 未知
// （无 AVSEEK_SIZE 语义）；setAbortFlag/abortRequested 接缝与 HttpSource 完全同构，
// AudioStreamDecoder 的注入链零改动。
//
// 构造期做一次性的头部嗅探（只读、短超时、与直通 open 同一 protocol_whitelist），
// 结果供分派层做内容层清单判定与 VOD 特征检查（#143 契约 §2）。
// 嗅探失败只反映在 initCheck 上，取消接缝不受影响。
// 所有 URL 解析与 FFmpeg 细节封在本 .cpp 内，头文件不暴露平台类型。
class UrlSource : public DataSource, public NonCopyable {
public:
    explicit UrlSource(const char *url);
    virtual ~UrlSource();

    virtual sdk_utils::status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual uint32_t flags() { return kIsHTTPBasedSource; }
    virtual std::string toString() { return m_name; }
    virtual std::string getUri() { return m_url; }

    // 构造期头部嗅探结果（内容层判定，#143 契约 §2）：
    // headLooksLikePlaylist —— 跳过 BOM/空白后以 #EXTM3U / <?xml 起始；
    // headHasVodFeatures    —— 含 #EXT-X-ENDLIST 或 #EXT-X-PLAYLIST-TYPE:VOD。
    // headLooksLikeLivePlaylist —— P3 LIVE 正面特征集（#152 D17）：#EXTM3U 起始
    //   （<?xml 归 .mpd / P4 拒绝，不放行）+ 无 ENDLIST + 无 PLAYLIST-TYPE:VOD
    //   + 至少一条分片 / variant 行（#EXTINF 或 #EXT-X-STREAM-INF）。
    //   PLAYLIST-TYPE:EVENT 归 LIVE 放行（#152 D18，已更正 #143 一条结论）。
    // master playlist（#EXT-X-STREAM-INF，本身无 ENDLIST）跟随首个 variant URI
    // 复查一次，语义是「判形态」（#152 / 规格 §9.3）：variant 有 VOD 特征 → VOD；
    // 仍无 ENDLIST / VOD → LIVE 放行；variant 取回失败 / 无法解析 → 明确拒绝
    // （不静默回退、不按「保守归 LIVE」放行）。
    bool headLooksLikePlaylist() const { return m_headPlaylist; }
    bool headHasVodFeatures() const { return m_headVod; }
    bool headLooksLikeLivePlaylist() const { return m_headLive; }

    // 扩展名层清单判定：URL 路径（剥 query/fragment）以 .m3u8 / .mpd 结尾。
    // 与 HttpSource 内部的 isPlaylistExtension 同一语义。
    static bool isPlaylistExtensionUrl(const std::string &url);

    // .mpd 专属判定（P4 未重开）：分派据此维持 P0 网络外拒绝。
    // .m3u8 不在此列：S2 已随 P2 重开（规格 §9.3），.m3u8 URL 由分派做
    // 头部嗅探 + VOD 特征区分（#143 契约 §2 / #144 review）。
    static bool isMpdExtensionUrl(const std::string &url);

private:
    bool sniffHead();
    bool fetchHead(const std::string &url, std::vector<uint8_t> &head);
    void classifyHead();

    std::string m_name;
    std::string m_url;
    std::vector<uint8_t> m_head;
    bool m_headPlaylist;
    bool m_headVod;
    bool m_headLive;
    sdk_utils::status_t m_initCheck;
};
