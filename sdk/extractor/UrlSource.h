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
    bool headLooksLikePlaylist() const { return m_headPlaylist; }
    bool headHasVodFeatures() const { return m_headVod; }

    // 扩展名层清单判定：URL 路径（剥 query/fragment）以 .m3u8 / .mpd 结尾。
    // 与 HttpSource 内部的 isPlaylistExtension 同一语义（P0 判定层级不动）。
    static bool isPlaylistExtensionUrl(const std::string &url);

private:
    bool sniffHead();

    std::string m_name;
    std::string m_url;
    std::vector<uint8_t> m_head;
    bool m_headPlaylist;
    bool m_headVod;
    sdk_utils::status_t m_initCheck;
};
