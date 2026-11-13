#pragma once

// P3 共享 HTTP mock（doc/live-source-spec.md §10.1，票 #159）：自 test_http_source.cpp
// 抽出（内容逐字节保留），供 ctest 内的 HlsUrlDirect / LiveSourceContract 套件与
// 不注册 ctest 的独立可执行 LiveSmoke 复用同一份 mock，避免两份实现漂移。
// P3 扩展：断流注入（failFromRequest / failRequests）与 ICY 直播端点（icyStreamPath，
// 仅当客户端请求 Icy-MetaData: 1 才回 Icy-* 头——去掉 HttpSource 的补头必红）。
//
// 全部流量走 127.0.0.1 上的进程私有端口（bind :0 由 OS 分配），素材来自
// RealMediaFixture::mediaPath("music.wav") 的固定本地副本，公网 URL 零依赖。

#include "../fixtures/RealMediaFixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sb_httpmock {

// P0 静态 HTTP 点播数据源的契约测试。
// 全部流量走 127.0.0.1 上的进程私有端口（bind :0 由 OS 分配），素材来自
// RealMediaFixture::mediaPath("music.wav") 的固定本地副本，公网 URL 零依赖。
// doctest 用例统一编进 sdk_unit_tests（-tse 排除串之外），保持轻量、不新增 gate。
namespace {

#ifdef _WIN32
using Socket = SOCKET;
using SockLen = int;
const Socket kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
inline void ensureWinsock()
{
    // 进程级一次性初始化，与 HttpSource 内部的引用计数 helper 由 Winsock
    // 自身的引用计数协调，互不干扰（静态对象在进程退出时配对清理）。
    static struct Guard {
        Guard() { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
        ~Guard() { WSACleanup(); }
    } guard;
    (void)guard;
}
#else
using Socket = int;
using SockLen = socklen_t;
const Socket kInvalidSocket = -1;
inline void closeSocket(Socket s) { ::close(s); }
inline void ensureWinsock() { }
#endif

inline void setRecvTimeout(Socket s)
{
#ifdef _WIN32
    DWORD ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline uint16_t netToHost16(const void *p)
{
    const unsigned char *b = (const unsigned char *)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

inline void hostToNet16(uint16_t v, void *p)
{
    unsigned char *b = (unsigned char *)p;
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)(v & 0xffu);
}

inline void setLoopbackAddr(struct sockaddr_in &addr)
{
    const unsigned char ip[4] = { 127, 0, 0, 1 };
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
}

std::string lowerCopy(std::string s)
{
    for (std::string::iterator it = s.begin(); it != s.end(); ++it) {
        *it = (char)std::tolower((unsigned char)*it);
    }
    return s;
}

// 只解析非负十进制整数，避免依赖平台相关的 strtoll / atoll。
bool parseInt64(const std::string &s, long long &out)
{
    if (s.empty()) return false;
    long long v = 0;
    for (std::string::const_iterator it = s.begin(); it != s.end(); ++it) {
        const char c = *it;
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

bool parseRange(const std::string &head, long long &start, long long &end)
{
    const std::string lower = lowerCopy(head);
    const size_t key = lower.find("range:");
    if (key == std::string::npos) return false;
    const size_t eol = head.find("\r\n", key);
    const std::string line = head.substr(
        key, (eol == std::string::npos) ? std::string::npos : eol - key);
    const size_t bytesIdx = lowerCopy(line).find("bytes=");
    if (bytesIdx == std::string::npos) return false;
    const std::string spec = line.substr(bytesIdx + 6);
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    return parseInt64(spec.substr(0, dash), start)
        && parseInt64(spec.substr(dash + 1), end);
}

std::vector<uint8_t> loadFixtureBytes()
{
    RealMediaFixture fixture;
    const std::string path = fixture.mediaPath("music.wav");
    std::vector<uint8_t> bytes;
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return bytes;
    uint8_t buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(fp);
    return bytes;
}

const std::vector<uint8_t> &fixtureBytes()
{
    static const std::vector<uint8_t> bytes = loadFixtureBytes();
    return bytes;
}
// ICY 直播端点素材：mp3 字节（wav 会被探测成有限时长，不符合「无限流」语义）。
// 中文名素材经 FileSource::readAt 读——FileSource 内部经 Utf8Path 转宽字符，
// 与生产链路同一条路径解析（#14），不用 std::fopen。
inline const std::vector<uint8_t> &mp3FixtureBytes()
{
    static const std::vector<uint8_t> bytes = []() {
        RealMediaFixture fixture;
        std::vector<uint8_t> out;
        // 像我这样的人-毛不易.128.mp3（源码内 UTF-8 转义，FileSource 内部转宽字符）
        FileSource source(
            fixture.mediaPath("\xE5\x83\x8F\xE6\x88\x91\xE8\xBF\x99\xE6\xA0\xB7\xE7\x9A\x84\xE4\xBA\xBA-\xE6\xAF\x9B\xE4\xB8\x8D\xE6\x98\x93.128.mp3").c_str());
        if (source.initCheck() != sdk_utils::OK) return out;
        out.resize(256 * 1024);
        const ssize_t n = source.readAt(0, out.data(), out.size());
        if (n <= 0) {
            out.clear();
        } else {
            out.resize(static_cast<size_t>(n));
        }
        return out;
    }();
    return bytes;
}

// P3 窗口化 LIVE 的 segment 素材（票 #159）：独立完整 wav = 44 字节头 +
// 32KB PCM 环回切片。fmt 字段从 fixture 头解析（声道/采样率/位深），
// 不硬编码素材参数。返回空 = 素材不足（测试显式红）。
inline std::vector<uint8_t> liveSegmentBytes(const std::vector<uint8_t> &wav, int seq)
{
    std::vector<uint8_t> out;
    if (wav.size() < 44) return out;
    const uint32_t pcmTotal = (uint32_t)wav[40] | ((uint32_t)wav[41] << 8)
                            | ((uint32_t)wav[42] << 16) | ((uint32_t)wav[43] << 24);
    if (pcmTotal == 0 || 44 + (size_t)pcmTotal > wav.size()) return out;
    const uint16_t channels = (uint16_t)wav[22] | ((uint16_t)wav[23] << 8);
    const uint32_t rate = (uint32_t)wav[24] | ((uint32_t)wav[25] << 8)
                        | ((uint32_t)wav[26] << 16) | ((uint32_t)wav[27] << 24);
    const uint16_t bits = (uint16_t)wav[34] | ((uint16_t)wav[35] << 8);
    if (channels == 0 || rate == 0 || bits == 0) return out;

    // 段音频长度 5s（16kHz 单声道 s16 下 160KB）：必须盖过 find_stream_info 的
    // max_analyze_duration（4s）——扫描期只在首段上凑 pts 跨度，若段音频短于
    // 分析窗口，扫描会无限拉段挂死。EXTINF 仍标 0.5s：reload 节奏不受段音频
    // 实际长度影响，保持窗口滑动与测试节奏。
    const uint32_t chunk = 163840;
    const uint64_t start = ((uint64_t)(unsigned)seq * chunk) % pcmTotal;
    const uint16_t blockAlign = (uint16_t)(channels * bits / 8);
    const uint32_t byteRate = rate * blockAlign;
    // data 块声明极大长度：hls.c 对整条 LIVE 流只建一个 demuxer 会话（首段探测
    // 格式，pls->pb 由 read_data 跨段续喂），data 块一旦耗尽，wav demuxer 会
    // 对后续所有段永远立即 EOF（实测：段被反复拉取但零出包）。声明不限长的
    // data = LIVE 无限供给语义；每个段头部仍带完整 wav 头，使重连后的新会话
    // 可以从任意序号段探测起步，后续段的前 44 字节被当作 PCM 噪声消费（无影响）。
    const uint32_t riffSize = 0x7FFFFFF8u;   // = dataSize + 8
    const uint32_t dataSize = 0x7FFFFFF0u;

    out.reserve(44 + chunk);
    // 44 字节标准 wav 头位型："RIFF"+size+“WAVE”+“fmt ”+fmt 区 20 字节+“data”+size。
    // 段与段之间逐字节独立构造，头错位会让 wav demuxer 在 PCM 里乱扫第二个
    // 'fmt ' tag（实测 found more than one 'fmt ' tag）→ 段被全部跳过。
    static const char magic[]
        = "RIFF____WAVEfmt ____________________data____";   // 16+20+4+4 = 44
    out.insert(out.end(), magic, magic + 44);
    std::memcpy(out.data() + 4, &riffSize, 4);
    { const uint32_t fmtSize = 16; std::memcpy(out.data() + 16, &fmtSize, 4); }   // fmt 块大小恒 16
    { const uint16_t fmt = 1; std::memcpy(out.data() + 20, &fmt, 2); }
    std::memcpy(out.data() + 22, &channels, 2);
    std::memcpy(out.data() + 24, &rate, 4);
    std::memcpy(out.data() + 28, &byteRate, 4);
    std::memcpy(out.data() + 32, &blockAlign, 2);
    std::memcpy(out.data() + 34, &bits, 2);
    std::memcpy(out.data() + 40, &dataSize, 4);
    out.resize(44 + chunk);
    for (uint32_t i = 0; i < chunk; ++i) {
        out[44 + i] = wav[44 + (size_t)((start + i) % pcmTotal)];
    }
    return out;
}

// 本地最小 HTTP/1.1 Range server：bind 127.0.0.1:0 拿进程私有端口，
// 每个连接处理一次请求后关闭（Connection: close）。析构置 stop 并自连接
// 唤醒阻塞的 accept，再 join，保证 ctest -j 下不残留线程 / 端口。
class MockHttpServer {
public:
    struct Config {
        bool emitIcy;
        bool omitAcceptRanges;
        bool acceptRangesNone;
        bool ignoreRange;
        bool fakeTotalOnTail;
        bool growContentLength;
        bool playlistBody;
        bool playlistBodyAsRange;
        bool growTotalAcrossRequests;
        bool hangAfterHeaders;
        int  hangFromRequest;
        int  redirects;
        std::string redirectLocation;
        // P2-1（issue #144）：请求路径（剥 query）命中 playlistPath 时返回 playlistText
        // （200 + application/vnd.apple.mpegurl），其余路径仍返回素材字节（供裸 wav segment）。
        // P2-2（issue #145）：playlistMap 支持一个 mock 内多级清单端点
        // （master → variant 各自路径独立应答），优先于单路径 playlistPath。
        std::string playlistPath;
        std::string playlistText;
        std::map<std::string, std::string> playlistMap;
        // 连续流素材模式：hls demuxer 对同一 playlist 只建一个 segment demuxer，
        // 全部 segment 经内部 read_data 流式续入；裸 wav demuxer 读到自己声明的
        // data 块末尾即 EOF，多 segment 必须拼成一条连续 PCM 流再按字节切给
        // 各 segment（seg0 = 头[data 声明两倍长] + 前半；seg1 = 后半）。
        bool hlsContinuousStream;
        // P3（票 #159 / 规格 §10.1 ①②③）：
        // failFromRequest / failRequests —— 从第 failFromRequest 个请求（0-based）起
        //   连续 failRequests 个请求回 404 并断开，之后恢复正常；覆盖 §5.3 两个失败面
        //   （playlist 重载失败 / segment 404 → 提取器以 EOF/失败收场）。
        // icyStreamPath —— 非空时启用 ICY 直播端点：仅当客户端请求带
        //   Icy-MetaData: 1 才回 Icy-* 头（真实 shoutcast 同构，去掉 HttpSource
        //   的补头必红）；body = mp3 字节无限推送，不回 EOF。
        // P3 窗口化 LIVE 模式（票 #159 / 规格 §10.1 ①）：liveWindowPath 非空时启用——
        // 每次取回该路径回一个 2 段滑动窗口（#EXT-X-MEDIA-SEQUENCE 递进，取回一次
        // 序号 +1），segment 以 seg<N>.ts 供独立完整 wav（fixture PCM 环回切片）。
        // demuxer 每次重载都能取到「下一个」分片 → 连续供给、不主动结束，
        // 复现真实 LIVE 的窗口推进节奏（研究文档 #148 §3 实测同构）。
        std::string liveWindowPath;
        int failFromRequest;
        int failRequests;
        std::string icyStreamPath;
        Config()
            : emitIcy(false), omitAcceptRanges(false),
              acceptRangesNone(false), ignoreRange(false),
              fakeTotalOnTail(false), growContentLength(false),
              playlistBody(false), playlistBodyAsRange(false),
              growTotalAcrossRequests(false), hangAfterHeaders(false),
              hangFromRequest(-1), redirects(0), redirectLocation(),
              playlistPath(), playlistText(), hlsContinuousStream(false),
              failFromRequest(-1), failRequests(0), icyStreamPath(),
              liveWindowPath() { }
    };

    MockHttpServer(const std::vector<uint8_t> &bytes, Config config = Config())
        : m_bytes(bytes), m_config(config), m_listen(kInvalidSocket), m_port(0),
          m_stop(false), m_requests(0)
    {
        ensureWinsock();
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen == kInvalidSocket) return;
        int reuse = 1;
        setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&reuse, sizeof(reuse));
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        setLoopbackAddr(addr);
        addr.sin_port = 0;
        if (bind(m_listen, (struct sockaddr *)&addr, (int)sizeof(addr)) != 0) {
            closeSocket(m_listen);
            m_listen = kInvalidSocket;
            return;
        }
        if (listen(m_listen, 8) != 0) {
            closeSocket(m_listen);
            m_listen = kInvalidSocket;
            return;
        }
        struct sockaddr_in bound;
        std::memset(&bound, 0, sizeof(bound));
        SockLen len = (SockLen)sizeof(bound);
        if (getsockname(m_listen, (struct sockaddr *)&bound, &len) == 0) {
            m_port = netToHost16(&bound.sin_port);
        }
        if (m_port == 0) {   // 拿不到端口就不启动线程，避免析构 join 挂死
            closeSocket(m_listen);
            m_listen = kInvalidSocket;
            return;
        }
        if (!m_config.icyStreamPath.empty()) {
            m_mp3Bytes = mp3FixtureBytes();
        }
        m_thread = std::thread(&MockHttpServer::serve, this);
    }

    ~MockHttpServer()
    {
        m_stop.store(true);
        Socket wake = ::socket(AF_INET, SOCK_STREAM, 0);
        if (wake != kInvalidSocket) {
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            setLoopbackAddr(addr);
            hostToNet16((uint16_t)m_port, &addr.sin_port);
            ::connect(wake, (struct sockaddr *)&addr, (int)sizeof(addr));
            closeSocket(wake);
        }
        if (m_thread.joinable()) m_thread.join();
        if (m_listen != kInvalidSocket) closeSocket(m_listen);
    }

    int port() const { return m_port; }
    int requestCount() const { return m_requests.load(); }
    bool hangReady() const { return m_hangReady.load(std::memory_order_acquire); }

    // P3 事件计数判据（票 #159）：按请求路径计数（剥 query 后的 reqPath）。
    // 重开次数的观测锚点：重连的 open 与 demuxer 的 reload 都打 playlist 路径，
    // 段请求打 seg<N>.ts 路径——两者组合可以把「重载」与「重开」区分开。
    long long requestCountFor(const std::string &path) const
    {
        std::lock_guard<std::mutex> lock(m_countMutex);
        std::map<std::string, long long>::const_iterator it = m_pathCounts.find(path);
        return it == m_pathCounts.end() ? 0 : it->second;
    }
    // 404 注入已服务的次数：预算耗尽判据 = 停滞触发 1 次 + 3 次重开。
    long long failCount() const { return m_failCount.load(); }

    // P3 断流注入 v2（票 #159 事件计数判据）：按「第 N 次 playlist 取回」注入 404，
    // 与请求总数解耦。扫描期 find_stream_info 的 reload 次数随素材分析时长浮动，
    // 无法在服务启动前用请求序号锚定「播放期首个 reload」；seq 是全局递增的
    // 取回序号，测试在扫描完成后用实测取回数（requestCountFor(livePath)）设定
    // 窗口起点，首个播放期 reload 的 seq 恰好等于该实测值。运行时可改（原子）。
    void setPlaylistFailWindow(int fromSeq, int count)
    {
        m_failPlaylistFromSeq.store(fromSeq, std::memory_order_relaxed);
        m_failPlaylistFetches.store(count, std::memory_order_relaxed);
    }

    std::string url(const std::string &path) const
    {
        return "http://127.0.0.1:" + std::to_string(m_port) + path;
    }

private:
    void serve()
    {
        while (true) {
            Socket client = ::accept(m_listen, nullptr, nullptr);
            if (client == kInvalidSocket) break;
            if (m_stop.load()) {
                closeSocket(client);
                break;
            }
            setRecvTimeout(client);
            handle(client);
            closeSocket(client);
        }
    }

    void handle(Socket client)
    {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, (size_t)n);
            if (req.size() > 64 * 1024) return;
        }
        const int index = m_requests.fetch_add(1);
        if (index < m_config.redirects) {
            const std::string body = "redirecting";
            const std::string location = m_config.redirectLocation.empty()
                ? std::string("/final.wav") : m_config.redirectLocation;
            const std::string resp = "HTTP/1.1 302 Found\r\n"
                                     "Location: " + location + "\r\n"
                                     "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                     "Connection: close\r\n\r\n" + body;
            sendAll(client, resp);
            return;
        }
        // P2-1/P2-2 清单端点：命中 playlistMap / playlistPath（剥 query）返回
        // 200 + mpegurl + 文本，未命中路径由默认分支按素材字节应答
        // （hls demuxer 以相对 URI 拉 segment）。
        std::string reqPath;
        {
            const size_t sp1 = req.find(' ');
            const size_t sp2 = (sp1 == std::string::npos)
                ? std::string::npos : req.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                reqPath = req.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            const size_t q = reqPath.find('?');
            if (q != std::string::npos) reqPath = reqPath.substr(0, q);
        }
        {
            std::lock_guard<std::mutex> lock(m_countMutex);
            ++m_pathCounts[reqPath];
        }
        // P3 断流注入（规格 §10.1 ②）：按请求序号计数，不分路径——playlist 重载
        // 与 segment 请求都会被命中，覆盖「提取器以 EOF/失败收场」的两个失败面。
        if (m_config.failRequests > 0 && index >= m_config.failFromRequest
            && index < m_config.failFromRequest + m_config.failRequests) {
            m_failCount.fetch_add(1);
            const std::string resp = "HTTP/1.1 404 Not Found\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n\r\n";
            sendAll(client, resp);
            return;
        }
        // P3 窗口化 LIVE（票 #159）：清单端点每次取回回当前 2 段窗口并推进序号；
        // segment 端点按序号回独立完整 wav（fixture PCM 环回切片）。
        if (!m_config.liveWindowPath.empty()) {
            if (reqPath == m_config.liveWindowPath) {
                const int seq = m_liveSeq.fetch_add(1);
                // 断流注入 v2：命中 [fromSeq, fromSeq+count) 的取回回 404（不计入
                // 请求序号注入面，两者互不干扰）。取回本身已推进 m_liveSeq。
                if (m_failPlaylistFetches.load(std::memory_order_relaxed) > 0) {
                    const int from = m_failPlaylistFromSeq.load(std::memory_order_relaxed);
                    const int count = m_failPlaylistFetches.load(std::memory_order_relaxed);
                    if (seq >= from && seq < from + count) {
                        m_failCount.fetch_add(1);
                        const std::string resp = "HTTP/1.1 404 Not Found\r\n"
                                                 "Content-Length: 0\r\n"
                                                 "Connection: close\r\n\r\n";
                        sendAll(client, resp);
                        return;
                    }
                }
                std::string text = "#EXTM3U\r\n"
                                   "#EXT-X-VERSION:3\r\n"
                                   "#EXT-X-TARGETDURATION:1\r\n"
                                   "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(seq) + "\r\n"
                                   "#EXTINF:0.5,\r\nseg" + std::to_string(seq) + ".ts\r\n"
                                   "#EXTINF:0.5,\r\nseg" + std::to_string(seq + 1) + ".ts\r\n";
                std::string resp = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/vnd.apple.mpegurl\r\n"
                                   "Content-Length: " + std::to_string(text.size()) + "\r\n"
                                   "Connection: close\r\n\r\n";
                resp += text;
                sendAll(client, resp);
                return;
            }
            // seg<N>.ts（N 非负十进制，可带前导 '/'）：独立完整 wav = 44 字节头 +
            // 160KB PCM 环回切片（段音频 5s，盖过 find_stream_info 的 4s 分析窗）。
            // 请求路径恒带前导斜杠（http://host/segN.ts，reqPath 原样
            // 入表、键也是 "/segN.ts"），所以先剥斜杠再匹配——否则落到兜底分支回
            // 整份 fixture，窗口序号与消费速度失步，FFmpeg 陷入无包重载死循环。
            std::string segPath = reqPath;
            if (!segPath.empty() && segPath[0] == '/') segPath.erase(0, 1);
            if (segPath.size() > 6 && segPath.compare(0, 3, "seg") == 0
                && segPath.compare(segPath.size() - 3, 3, ".ts") == 0) {
                long long seqVal = -1;
                if (parseInt64(segPath.substr(3, segPath.size() - 6), seqVal) && seqVal >= 0) {
                    const std::vector<uint8_t> body = liveSegmentBytes(m_bytes, (int)seqVal);
                    if (!body.empty()) {
                        std::string resp = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: video/mp2t\r\n"
                                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                           "Connection: close\r\n\r\n";
                        resp.append((const char *)body.data(), body.size());
                        sendAll(client, resp);
                        return;
                    }
                }
            }
        }
        // P3 ICY 直播端点（规格 §10.1 ③）：Icy-* 头按客户端请求条件回（可红可绿），
        // icy-metaint 恒在（真实 shoutcast 同构，票 #159 ①），body 为 mp3 字节
        // 无限推送、不回 EOF（裸无限字节流）。
        if (!m_config.icyStreamPath.empty() && reqPath == m_config.icyStreamPath) {
            const bool metaRequested
                = lowerCopy(req).find("icy-metadata: 1") != std::string::npos;
            std::string icyResp = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: audio/mpeg\r\n"
                                  "Connection: close\r\n";
            if (metaRequested) icyResp += "icy-name: mock-radio\r\n";
            icyResp += "icy-metaint: 8192\r\n\r\n";
            sendAll(client, icyResp);
            // 每 8192 字节音频后插一个 1 字节「无元数据」块（0x00）：FFmpeg http
            // 协议按 metaint 剥离元数据块，demuxer 收到干净 mp3。
            const size_t kMetaInt = 8192;
            size_t sent = 0;
            const std::string metaBlock("\x00", 1);
            while (!m_stop.load() && !m_mp3Bytes.empty()) {
                const size_t once = std::min(kMetaInt - (sent % kMetaInt), m_mp3Bytes.size() - sent);
                std::string chunk((const char *)m_mp3Bytes.data() + sent, once);
                if (!sendAllGuarded(client, chunk)) break;
                sent += once;
                if (sent % kMetaInt == 0) {
                    if (!sendAllGuarded(client, metaBlock)) break;
                }
                if (sent >= m_mp3Bytes.size()) sent = 0;   // 环回：无限循环推送
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return;
        }
        std::string hlsText;
        if (!reqPath.empty()) {
            std::map<std::string, std::string>::const_iterator it
                = m_config.playlistMap.find(reqPath);
            if (it != m_config.playlistMap.end()) {
                hlsText = it->second;
            } else if (reqPath == m_config.playlistPath) {
                hlsText = m_config.playlistText;
            }
        }
        if (!hlsText.empty()) {
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/vnd.apple.mpegurl\r\n"
                               "Content-Length: " + std::to_string(hlsText.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
            resp += hlsText;
            sendAll(client, resp);
            return;
        }
        // 清单式响应：忽略 Range，返回整份 #EXTM3U 文本（200），用于验证
        // 无扩展名 URL 命中清单内容时被静态点播门禁明确拒绝。
        if (m_config.playlistBody) {
            const std::string playlist =
                "#EXTM3U\r\n"
                "#EXT-X-VERSION:3\r\n"
                "#EXT-X-TARGETDURATION:10\r\n"
                "#EXTINF:10.0,\r\n"
                "segment0.ts\r\n"
                "#EXT-X-ENDLIST\r\n";
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/vnd.apple.mpegurl\r\n"
                               "Content-Length: " + std::to_string(playlist.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
            resp += playlist;
            sendAll(client, resp);
            return;
        }
        // 连续流素材：seg0 = wav 头（data 声明两份 PCM 总长）+ 前半数据；
        // seg1 = 其余部分（重复一份 PCM 补满声明长度）。wav demuxer 跨段连续供数。
        if (m_config.hlsContinuousStream
            && (reqPath.find("seg0") != std::string::npos
                || reqPath.find("seg1") != std::string::npos)) {
            const bool first = reqPath.find("seg0") != std::string::npos;
            const uint32_t dataSize = (m_bytes.size() >= 44)
                ? (uint32_t)m_bytes[40] | ((uint32_t)m_bytes[41] << 8)
                  | ((uint32_t)m_bytes[42] << 16) | ((uint32_t)m_bytes[43] << 24)
                : 0;
            const uint32_t pcmTotal = (dataSize > 0 && 44 + (size_t)dataSize <= m_bytes.size())
                ? dataSize : 0;
            if (pcmTotal == 0) return;
            const uint32_t half = pcmTotal / 2;
            std::vector<uint8_t> body;
            if (first) {
                std::vector<uint8_t> header(m_bytes.begin(), m_bytes.begin() + 44);
                const uint32_t riffSize = 36 + pcmTotal * 2;
                const uint32_t declared = pcmTotal * 2;
                std::memcpy(header.data() + 4, &riffSize, 4);
                std::memcpy(header.data() + 40, &declared, 4);
                body.swap(header);
                body.insert(body.end(), m_bytes.begin() + 44, m_bytes.begin() + 44 + half);
            } else {
                body.assign(m_bytes.begin() + 44 + half, m_bytes.begin() + 44 + pcmTotal);
                body.insert(body.end(), m_bytes.begin() + 44, m_bytes.begin() + 44 + half);
            }
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: video/mp2t\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
            resp.append((const char *)body.data(), body.size());
            sendAll(client, resp);
            return;
        }
        long long start = 0, end = 0;
        const bool hasRange = parseRange(req, start, end);
        const long long total = (long long)m_bytes.size();
        if (m_config.ignoreRange || !hasRange) {
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: " + std::to_string(total) + "\r\n"
                               "Connection: close\r\n\r\n";
            resp.append((const char *)m_bytes.data(), (size_t)total);
            sendAll(client, resp);
            return;
        }
        if (start < 0) start = 0;
        if (end >= total) end = total - 1;
        if (start > end) start = end;
        const long long length = end - start + 1;

        // 清单类以 206 承载：把文件头 patch 成 #EXTM3U 起始并带清单 Content-Type，
        // 用于验证 probe 在 206 分支内对清单内容的明确拒绝（不依赖扩展名）。
        // body 仍取 m_bytes 的切片、total 不变，保证去掉该检查后其余校验仍可通过。
        std::vector<uint8_t> patched;
        const uint8_t *bodyData = m_bytes.data();
        if (m_config.playlistBodyAsRange) {
            patched.assign(m_bytes.begin(), m_bytes.end());
            const char magic[] = "#EXTM3U";
            for (size_t i = 0; i < sizeof(magic) - 1 && i < patched.size(); ++i) {
                patched[i] = (uint8_t)magic[i];
            }
            bodyData = patched.data();
        }

        long long declaredTotal = total;
        if (m_config.fakeTotalOnTail && length == 1 && start == total - 1) {
            declaredTotal = total + 7;   // 伪长度：二次校验 total 与首次不一致
        }
        // 伪长度「Content-Length 随数据增长」：第 3 次请求起 declaredTotal 增长，
        // 用于验证构造期第三次探测（verifyHeadAgain）的有效性。
        if (m_config.growTotalAcrossRequests && index >= 2) {
            declaredTotal = total + 7;
        }
        // 声明长度大于实际 body：模拟连接不自然终止的伪长度源。
        const long long declaredLength =
            m_config.growContentLength ? (length + 64) : length;
        std::string resp = "HTTP/1.1 206 Partial Content\r\n";
        if (m_config.acceptRangesNone) {
            resp += "Accept-Ranges: none\r\n";
        } else if (!m_config.omitAcceptRanges) {
            resp += "Accept-Ranges: bytes\r\n";
        }
        resp += "Content-Range: bytes " + std::to_string(start) + "-"
                + std::to_string(end) + "/" + std::to_string(declaredTotal) + "\r\n";
        resp += "Content-Length: " + std::to_string(declaredLength) + "\r\n";
        if (m_config.playlistBodyAsRange) {
            resp += "Content-Type: application/vnd.apple.mpegurl\r\n";
        }
        resp += "Connection: close\r\n";
        if (m_config.emitIcy) resp += "Icy-Name: mock-radio\r\n";
        resp += "\r\n";
        // 挂起开关（构造期设定、运行期只读）：第 hangFromRequest 次请求起
        // 只发响应头、不发 body 并保持连接打开，直到析构置 m_stop 才退出。
        // 前 3 次探测请求（seq 1..3）正常返回，第 4 次 readAt 才挂起。
        const int seq = index + 1;
        if (m_config.hangAfterHeaders && seq >= m_config.hangFromRequest) {
            // 只读就绪信号：请求已收到、响应头尚未发出，client 此刻正阻塞在
            // 读响应头的 recv 上。仅 mock server 线程写、测试控制线程读，
            // release/acquire 配对；留一小段窗口让控制线程先发布取消，再发头，
            // 使取消确定性地落在三处 recv 取消检查点（而非连接分片检查点）。
            m_hangReady.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            sendAll(client, resp);
            while (!m_stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return;
        }
        if (length > 0) {
            resp.append((const char *)bodyData + start, (size_t)length);
        }
        sendAll(client, resp);
    }

    static bool sendAll(Socket s, const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            const int n = ::send(s, data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    // ICY 无限推流专用：裸 sendAll 在读端停拉（TCP 背压、缓冲写满）时永久阻塞，
    // mock 线程卡死 → 析构 join 卡死 → 用例超时。select 可写探测 + 1s 超时：
    // 读端在拉就尽快推；1 s 推不出去就断开（客户端下一读表现为连接关闭），
    // mock 线程与析构永不卡死。LIVE 消费端把「连接关闭」当供给停滞，属规格内语义。
    static bool sendAllGuarded(Socket s, const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            const int ready = ::select((int)s + 1, nullptr, &wfds, nullptr, &tv);
            if (ready <= 0 || !FD_ISSET(s, &wfds)) return false;
            const int n = ::send(s, data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    const std::vector<uint8_t> &m_bytes;
    const Config m_config;
    std::vector<uint8_t> m_mp3Bytes;
    Socket m_listen;
    int m_port;
    std::atomic<bool> m_stop;
    std::atomic<int> m_requests;
    std::atomic<bool> m_hangReady{false};
    std::thread m_thread;
    // P3 事件计数：按路径的请求计数（互斥保护，读在测试线程）+ 404 计数 +
    // 窗口化 LIVE 的滑动序号（每次清单取回 +1）。
    mutable std::mutex m_countMutex;
    std::map<std::string, long long> m_pathCounts;
    std::atomic<long long> m_failCount{0};
    std::atomic<int> m_liveSeq{0};
    // 断流注入 v2 的窗口（setPlaylistFailWindow 写、handle 线程读）。
    std::atomic<int> m_failPlaylistFromSeq{-1};
    std::atomic<int> m_failPlaylistFetches{0};
};

// 轮询 mock server 的只读就绪信号：确认 client 已阻塞在读响应头的 recv 上。
// 最多等 2s（每次 sleep 5ms）；只在测试控制线程读，与 server
// 线程的 release store 配对，绝不落解码线程 / 判据主循环。
bool waitForHangReady(const MockHttpServer &server)
{
    for (int i = 0; i < 400; ++i) {
        if (server.hangReady()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace（匿名：block 携带的 namespace { 在此闭合）
} // namespace sb_httpmock
