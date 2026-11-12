#include "UrlSource.h"
#include "AbortInterrupt.hpp"
#include "LogWrapper.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/dict.h"
}

#include <cctype>
#include <cstring>

#define LOG_TAG "UrlSource"

using namespace sdk_utils;

namespace {

// 头部嗅探上限：清单头部字节足够做清单类与 VOD 特征判定。到达上限即停
// （#EXT-X-ENDLIST 在超长清单尾部时嗅探不到 → 按无 VOD 特征拒绝）：
// 宁可把超长 VOD 误拒成 LIVE，也不放行无界读取 —— 语义保守（#143 契约 §2）。
const size_t kHeadSniffMaxBytes = 512 * 1024;
const int kHeadSniffTimeoutUs = 3 * 1000 * 1000;   // 3s，与 HttpSource socket 超时口径一致

std::string urlPath(const std::string &url)
{
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return url;
    const size_t slash = url.find('/', schemeEnd + 3);
    std::string path = (slash == std::string::npos) ? std::string("/") : url.substr(slash);
    const size_t frag = path.find('#');
    if (frag != std::string::npos) path = path.substr(0, frag);
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    if (path.empty()) path = "/";
    return path;
}

std::string pathExtensionLower(const std::string &path)
{
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return std::string();
    if (slash != std::string::npos && dot < slash) return std::string();
    std::string ext;
    for (size_t i = dot; i < path.size(); ++i) {
        ext += (char)std::tolower((unsigned char)path[i]);
    }
    return ext;
}

bool containsAscii(const std::vector<uint8_t> &bytes, const char *text)
{
    const size_t len = std::strlen(text);
    if (bytes.size() < len) return false;
    for (size_t i = 0; i + len <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, text, len) == 0) return true;
    }
    return false;
}

} // namespace

bool UrlSource::isPlaylistExtensionUrl(const std::string &url)
{
    const std::string ext = pathExtensionLower(urlPath(url));
    return ext == ".m3u8" || ext == ".mpd";
}

UrlSource::UrlSource(const char *url)
    : m_headPlaylist(false)
    , m_headVod(false)
    , m_initCheck(NO_INIT)
{
    m_name = url ? std::string(url) : std::string("<null>");
    m_url  = m_name;
    LOG_DEBUG(LOG_TAG, "%s", m_name.c_str());
    if (url == nullptr || url[0] == '\0') {
        LOG_ERROR(LOG_TAG, "empty url");
        return;
    }
    if (!sniffHead()) {
        // 嗅探失败只落 initCheck：分派层据此回落既有 P0 路径（HttpSource 探测判定），
        // 不在这里新增拒绝出口（#143 契约 §6 的错误分层）。
        LOG_ERROR(LOG_TAG, "head sniff failed: %s", m_url.c_str());
        return;
    }
    m_initCheck = OK;
    LOGI("url source ready, headBytes=%zu playlist=%d vod=%d", m_head.size(),
         m_headPlaylist ? 1 : 0, m_headVod ? 1 : 0);
}

UrlSource::~UrlSource() { }

sdk_utils::status_t UrlSource::initCheck() const
{
    return m_initCheck;
}

ssize_t UrlSource::readAt(off64_t offset, void *data, size_t size)
{
    (void)offset;
    (void)data;
    (void)size;
    // 契约 #143 §4：直通路径不经 readAt 供数（FFmpeg 原生 URL 层自取），
    // readAt 一律失败、size 未知（无 AVSEEK_SIZE 语义）。
    return -1;
}

bool UrlSource::sniffHead()
{
    avformat_network_init();   // 幂等：原生 URL 层的网络协议初始化

    AVIOContext *avio = nullptr;
    AVDictionary *opts = nullptr;
    // 契约 #143 §1：网络源只许出网络。嗅探与直通 open 用同一白名单
    // （http,tcp，不含 file/https/crypto）。
    av_dict_set(&opts, "protocol_whitelist", "http,tcp", 0);
    av_dict_set(&opts, "timeout", "3000000", 0);   // 只读、短超时（微秒）
    AVIOInterruptCB cb;
    cb.callback = &sbAbortInterruptCallback;
    cb.opaque   = this;
    const int ret = avio_open2(&avio, m_url.c_str(), AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "avio_open2 failed: %d", ret);
        return false;
    }

    bool aborted = false;
    uint8_t buf[4096];
    while (m_head.size() < kHeadSniffMaxBytes) {
        if (sbAbortRequested(this)) {
            aborted = true;
            break;
        }
        const int n = avio_read(avio, buf, (int)sizeof(buf));
        if (n <= 0) break;   // EOF（清单通常远小于上限）或读错误
        m_head.insert(m_head.end(), buf, buf + (size_t)n);
    }
    avio_closep(&avio);
    if (aborted) {
        LOG_ERROR(LOG_TAG, "head sniff cancelled by abort flag");
        return false;
    }

    // 内容嗅探层（与 HttpSource 的 looksLikePlaylist 内容分支同一语义）：
    // 跳过 UTF-8 BOM 与空白后，以 #EXTM3U / <?xml 起始即清单类。
    // Content-Type 分支不经此路（avio 不暴露响应头）：仅 CT 命中的清单
    // 落回 HttpSource 的探测判定，仍是明确拒绝，不静默（语义保守）。
    size_t i = 0;
    if (m_head.size() >= 3 && m_head[0] == 0xEF && m_head[1] == 0xBB && m_head[2] == 0xBF) {
        i = 3;
    }
    while (i < m_head.size()
           && (m_head[i] == ' ' || m_head[i] == '\t'
               || m_head[i] == '\r' || m_head[i] == '\n')) {
        ++i;
    }
    const char *body = (const char *)m_head.data() + i;
    const size_t bodyLen = m_head.size() - i;
    m_headPlaylist
        = (bodyLen >= 7 && std::memcmp(body, "#EXTM3U", 7) == 0)
          || (bodyLen >= 5 && std::memcmp(body, "<?xml", 5) == 0);
    if (m_headPlaylist) {
        // VOD 特征追加（#143 契约 §2）：#EXT-X-ENDLIST 或 #EXT-X-PLAYLIST-TYPE:VOD。
        // PLAYLIST-TYPE:EVENT 有界但增长，不满足「有界窗口」定义 → 无 VOD 特征 → 拒绝。
        m_headVod = containsAscii(m_head, "#EXT-X-ENDLIST")
                    || containsAscii(m_head, "#EXT-X-PLAYLIST-TYPE:VOD");
    }
    return true;
}
