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

// master playlist（#EXT-X-STREAM-INF）里首个 variant URI：紧随该 tag 的第一个
// 非注释、非空行（RFC 8216 §4.3.4.2）。
bool firstVariantUri(const std::vector<uint8_t> &head, std::string &uri)
{
    std::string text(head.begin(), head.end());
    size_t pos = 0;
    bool pending = false;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, (eol == std::string::npos)
                                                ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        while (!line.empty() && (line[line.size() - 1] == '\r'
                                 || line[line.size() - 1] == ' ')) {
            line.erase(line.size() - 1);
        }
        if (line.empty()) continue;
        if (line[0] == '#') {
            pending = (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0);
            continue;
        }
        if (pending) {
            uri = line;
            return true;
        }
    }
    return false;
}

// 相对 variant URI 解析：同 origin + 清单路径目录拼接；绝对 http(s) URL 原样
// 返回（https 变体会被白名单拒绝，语义保守）。
std::string resolveUrl(const std::string &base, const std::string &uri)
{
    if (uri.compare(0, 7, "http://") == 0 || uri.compare(0, 8, "https://") == 0) {
        return uri;
    }
    const size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return std::string();
    const size_t slash = base.find('/', schemeEnd + 3);
    if (slash == std::string::npos) return base + "/" + uri;
    const size_t lastSlash = base.rfind('/');
    const std::string dir = (lastSlash > slash)
        ? base.substr(slash, lastSlash - slash + 1) : std::string("/");
    return base.substr(0, slash) + dir + uri;
}

} // namespace

bool UrlSource::isPlaylistExtensionUrl(const std::string &url)
{
    const std::string ext = pathExtensionLower(urlPath(url));
    return ext == ".m3u8" || ext == ".mpd";
}

bool UrlSource::isMpdExtensionUrl(const std::string &url)
{
    return pathExtensionLower(urlPath(url)) == ".mpd";
}

UrlSource::UrlSource(const char *url)
    : m_headPlaylist(false)
    , m_headVod(false)
    , m_headLive(false)
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
    LOGI("url source ready, headBytes=%zu playlist=%d vod=%d live=%d", m_head.size(),
         m_headPlaylist ? 1 : 0, m_headVod ? 1 : 0, m_headLive ? 1 : 0);
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
    if (!fetchHead(m_url, m_head)) {
        return false;
    }
    classifyHead();

    // P3（#152 / 规格 §9.3）：master playlist 本身无 ENDLIST，VOD/LIVE 语义由
    // variant 承载。跟随首个 variant URI 复查一次，语义是「判形态」：variant 有
    // VOD 特征 → VOD；仍无 ENDLIST / VOD → LIVE 放行（正面特征满足时）；
    // **取回失败 / 无法解析 → 明确拒绝**（m_headLive=false，不静默回退、
    // 不按「保守归 LIVE」放行，依规格 §6.3 分派失败出口）。
    if (m_headPlaylist && !m_headVod && containsAscii(m_head, "#EXT-X-STREAM-INF")) {
        std::string variantUri;
        if (firstVariantUri(m_head, variantUri)) {
            const std::string variantUrl = resolveUrl(m_url, variantUri);
            std::vector<uint8_t> variantHead;
            if (!variantUrl.empty() && fetchHead(variantUrl, variantHead)) {
                m_head.swap(variantHead);
                classifyHead();
            } else {
                m_headLive = false;
            }
        } else {
            m_headLive = false;
        }
    }
    return true;
}

bool UrlSource::fetchHead(const std::string &url, std::vector<uint8_t> &head)
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
    const int ret = avio_open2(&avio, url.c_str(), AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOG_ERROR(LOG_TAG, "avio_open2 failed: %d", ret);
        return false;
    }

    bool aborted = false;
    uint8_t buf[4096];
    head.clear();
    while (head.size() < kHeadSniffMaxBytes) {
        if (sbAbortRequested(this)) {
            aborted = true;
            break;
        }
        const int n = avio_read(avio, buf, (int)sizeof(buf));
        if (n <= 0) break;   // EOF（清单通常远小于上限）或读错误
        head.insert(head.end(), buf, buf + (size_t)n);
    }
    avio_closep(&avio);
    if (aborted) {
        LOG_ERROR(LOG_TAG, "head sniff cancelled by abort flag");
        return false;
    }
    return true;
}

void UrlSource::classifyHead()
{
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
    const bool extm3u = bodyLen >= 7 && std::memcmp(body, "#EXTM3U", 7) == 0;
    const bool xml = bodyLen >= 5 && std::memcmp(body, "<?xml", 5) == 0;
    m_headPlaylist = extm3u || xml;
    if (m_headPlaylist) {
        // VOD 特征（#143 契约 §2）：#EXT-X-ENDLIST 或 #EXT-X-PLAYLIST-TYPE:VOD。
        m_headVod = containsAscii(m_head, "#EXT-X-ENDLIST")
                    || containsAscii(m_head, "#EXT-X-PLAYLIST-TYPE:VOD");
        // P3 LIVE 正面特征集（#152 D17/D18）：#EXTM3U 起始 + 无 ENDLIST
        // + 无 PLAYLIST-TYPE:VOD + 至少一条分片 / variant 行。只凭「有 EXTM3U
        // 且无 ENDLIST」不够——会把空清单 / 伪清单 / 坏清单放进来再无限重试。
        // PLAYLIST-TYPE:EVENT 归 LIVE 放行（D18）；<?xml 起始（.mpd/P4）不放行。
        // 已知误判面（规格 §9.3 显式登记）：超长清单尾部 ENDLIST 嗅探不到 →
        // VOD 误判为 LIVE；不放宽嗅探长度（会动 P2 判据数值）。
        m_headLive = extm3u && !m_headVod
                     && (containsAscii(m_head, "#EXTINF")
                         || containsAscii(m_head, "#EXT-X-STREAM-INF"));
    } else {
        m_headVod = false;
        m_headLive = false;
    }
}
