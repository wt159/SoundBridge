#include "../fixtures/RealMediaFixture.hpp"
#include "../fixtures/TestProcess.hpp"
#include <ExtractorFactory.h>
#include <HttpSource.h>
#include <MusicPlayList.h>
#include <UrlSource.h>
#include <doctest/doctest.h>
#include "AbortInterrupt.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstring>
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
        std::string playlistPath;
        std::string playlistText;
        Config()
            : emitIcy(false), omitAcceptRanges(false),
              acceptRangesNone(false), ignoreRange(false),
              fakeTotalOnTail(false), growContentLength(false),
              playlistBody(false), playlistBodyAsRange(false),
              growTotalAcrossRequests(false), hangAfterHeaders(false),
              hangFromRequest(-1), redirects(0), redirectLocation(),
              playlistPath(), playlistText() { }
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
        // P2-1 清单端点：命中 playlistPath（剥 query）返回 200 + mpegurl + 文本，
        // 未命中路径由默认分支按素材字节应答（hls demuxer 以相对 URI 拉 segment）。
        if (!m_config.playlistPath.empty()) {
            std::string reqPath;
            const size_t sp1 = req.find(' ');
            const size_t sp2 = (sp1 == std::string::npos)
                ? std::string::npos : req.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                reqPath = req.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            const size_t q = reqPath.find('?');
            if (q != std::string::npos) reqPath = reqPath.substr(0, q);
            if (reqPath == m_config.playlistPath) {
                std::string resp = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/vnd.apple.mpegurl\r\n"
                                   "Content-Length: "
                                   + std::to_string(m_config.playlistText.size()) + "\r\n"
                                   "Connection: close\r\n\r\n";
                resp += m_config.playlistText;
                sendAll(client, resp);
                return;
            }
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

    const std::vector<uint8_t> &m_bytes;
    const Config m_config;
    Socket m_listen;
    int m_port;
    std::atomic<bool> m_stop;
    std::atomic<int> m_requests;
    std::atomic<bool> m_hangReady{false};
    std::thread m_thread;
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

} // namespace

TEST_SUITE("HttpSource")
{
    TEST_CASE("static HTTP source reports initCheck OK")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
    }

    TEST_CASE("getSize returns OK and the exact file byte count")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        off64_t size = 0;
        REQUIRE(source.getSize(&size) == sdk_utils::OK);
        REQUIRE(size == (off64_t)bytes.size());
    }

    TEST_CASE("flags contains kIsHTTPBasedSource")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        const uint32_t f = source.flags();
        REQUIRE((f & DataSourceBase::kIsHTTPBasedSource) != 0);
    }

    TEST_CASE("readAt(0, ..., 16) matches the file header")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        uint8_t buf[16];
        const ssize_t n = source.readAt(0, buf, sizeof(buf));
        REQUIRE(n == 16);
        REQUIRE(std::memcmp(buf, bytes.data(), 16) == 0);
    }

    TEST_CASE("readAt(mid) matches file bytes at the same absolute offset")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 128);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        const off64_t mid = (off64_t)bytes.size() / 2;
        uint8_t buf[32];
        const ssize_t n = source.readAt(mid, buf, sizeof(buf));
        REQUIRE(n == 32);
        REQUIRE(std::memcmp(buf, bytes.data() + mid, 32) == 0);
    }

    TEST_CASE("readAt near the tail is clamped to the remaining bytes")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        uint8_t buf[100];
        const ssize_t n = source.readAt((off64_t)bytes.size() - 4, buf, sizeof(buf));
        REQUIRE(n == 4);
        REQUIRE(std::memcmp(buf, bytes.data() + bytes.size() - 4, 4) == 0);
    }

    TEST_CASE("readAt at or beyond total returns 0")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        uint8_t buf[16];
        REQUIRE(source.readAt((off64_t)bytes.size(), buf, sizeof(buf)) == 0);
        REQUIRE(source.readAt((off64_t)bytes.size() + 99, buf, sizeof(buf)) == 0);
    }

    TEST_CASE("toString returns the source URL")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        const std::string url = server.url("/music.wav");
        HttpSource source(url.c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        REQUIRE(source.toString() == url);
    }

    TEST_CASE("unsupported URLs are rejected without any networking")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);
        const std::string httpsUrl = "https://127.0.0.1:"
            + std::to_string(server.port()) + "/music.wav";
        const std::string m3u8Url = server.url("/list.m3u8");
        const std::string mpdUrl = server.url("/manifest.mpd");
        const std::string m3u8QueryUrl = server.url("/live.m3u8?token=abc");
        HttpSource a(httpsUrl.c_str());
        HttpSource b(m3u8Url.c_str());
        HttpSource c(mpdUrl.c_str());
        HttpSource d(m3u8QueryUrl.c_str());
        CHECK(a.initCheck() != sdk_utils::OK);
        CHECK(b.initCheck() != sdk_utils::OK);
        CHECK(c.initCheck() != sdk_utils::OK);
        CHECK(d.initCheck() != sdk_utils::OK);
        CHECK(server.requestCount() == 0);   // 明确拒绝且不联网
    }

    TEST_CASE("redirect is followed and the final response is probed")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.redirects = 1;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/start.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        off64_t size = 0;
        REQUIRE(source.getSize(&size) == sdk_utils::OK);
        REQUIRE(size == (off64_t)bytes.size());
        REQUIRE(server.requestCount() >= 3);   // 302 探测 + 206 探测 + 尾校验
    }

    TEST_CASE("Icy-* response header is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.emitIcy = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("missing Accept-Ranges is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.omitAcceptRanges = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("inconsistent Content-Range total is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.fakeTotalOnTail = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("Content-Range total growing on the third probe is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.growTotalAcrossRequests = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);   // 第三次探测 total 不一致即拒绝
    }

    TEST_CASE("server ignoring Range (200 full body) is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.ignoreRange = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);   // 探测要求 206，不降级为边下边播
    }

    TEST_CASE("explicit Accept-Ranges: none is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.acceptRangesNone = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("declared Content-Length larger than body is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.growContentLength = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
    }

    TEST_CASE("extension-less URL with native-format content is accepted")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        uint8_t buf[16];
        const ssize_t n = source.readAt(0, buf, sizeof(buf));
        REQUIRE(n == 16);
        REQUIRE(std::memcmp(buf, bytes.data(), 16) == 0);
    }

    TEST_CASE("extension-less URL serving playlist text is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistBody = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);   // 清单式响应明确拒绝，不静默回退
    }

    TEST_CASE("206 response with playlist Content-Type/body is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistBodyAsRange = true;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);   // 清单类 206 明确拒绝，不落到 magic sniff
    }

    TEST_CASE("redirect to a playlist target is explicitly rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.redirects = 1;
        cfg.redirectLocation = "/list.m3u8";
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/start.wav").c_str());
        CHECK(source.initCheck() != sdk_utils::OK);
        CHECK(server.requestCount() >= 1);   // 已处理重定向，不是构造期静默拒绝
    }

    TEST_CASE("cancelled read exits a hanging response promptly")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.hangAfterHeaders = true;
        cfg.hangFromRequest = 4;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);

        std::atomic<bool> flag(true);   // 取消信号先置位
        source.setAbortFlag(&flag);      // 复用 #135 的内部虚接缝
        auto fut = std::async(std::launch::async, [&] {
            char buf[16];
            return source.readAt(0, buf, sizeof(buf));
        });
        REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(fut.get() == -1);
    }

    // 对照组：与上面的取消用例共用同一挂起形态，差别只在唯一取消信号。
    // 不置取消时 readAt 不提前退出，等满数据源层 3s 读超时后正常失败；
    // 判据只用布尔结果，wait_for(2s) 仅为测试内部超时保护（非门禁数值阈值）。
    TEST_CASE("uncancelled hanging read falls back to read timeout")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.hangAfterHeaders = true;
        cfg.hangFromRequest = 4;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);

        std::atomic<bool> flag(false);   // 注入但不置取消，与取消用例形成对照
        source.setAbortFlag(&flag);
        auto fut = std::async(std::launch::async, [&] {
            char buf[16];
            return source.readAt(0, buf, sizeof(buf));
        });
        CHECK(fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready);
        CHECK(fut.get() == -1);   // 等满 3s 读超时后正常失败
    }

    // 必修 1：WAV 主路径的注入口。工厂按 ".wav" 命中 WAVExtractor，
    // getDataSource() 必须回传构造时的 HttpSource，取消注入才不落空。
    TEST_CASE("WAV extractor exposes its injected data source")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, "wav", true));
        REQUIRE(extractor != nullptr);
        CHECK(extractor->getDataSource() == &source);
    }

    // 必修 5：取消在 client 已进入 recv 之后才发布，必落在三处 recv 取消
    // 检查点之一（而非 connect 分片检查点），闭合 #138 登记的端到端缺口。
    TEST_CASE("cancelled read exits while blocked in recv")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.hangAfterHeaders = true;
        cfg.hangFromRequest = 4;
        MockHttpServer server(bytes, cfg);
        HttpSource source(server.url("/music.wav").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);

        std::atomic<bool> flag(false);
        source.setAbortFlag(&flag);
        auto fut = std::async(std::launch::async, [&] {
            char buf[16];
            return source.readAt(0, buf, sizeof(buf));
        });
        REQUIRE(waitForHangReady(server));   // 确认 client 已进 recv 挂起
        flag.store(true);                    // 取消此刻才发布 → 必落在 recv 循环
        REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(fut.get() == -1);
    }
}


// ---------------------------------------------------------------------------
// P0-3 端到端：URL 经既有播放列表字符串入口进 HttpSource；只验证扫描阶段的
// AudioSpec / 时长 / 字节，不引真实音频设备。端口 bind :0 由 OS 分配、进程私有，
// ctest -j 下互不干扰（无需额外响应头做 pid 隔离）。
// ---------------------------------------------------------------------------
using namespace sdk;

namespace {

// 回调把扫描阶段登记的「当前曲目属性」与错误码带出来，供断言观察。
struct DispatchCallback : public MusicPlayListCallback {
    std::vector<MusicPropertiesPtr> currentBufs;
    std::vector<ErrorCode> errors;

    void putMusicPlayListCurBuf(MusicPropertiesPtr p) override
    {
        currentBufs.push_back(p);
    }
    void updateMusicList(std::vector<MusicPropertiesPtr> &) override { }
    void onMusicPlayListError(ErrorCode code, const std::string &, int,
                              const std::string &) override
    {
        errors.push_back(code);
    }
};

// 复用 test_playlist.cpp 的构造 / 等待模式：addMusicWithNotify + future 串行加载。
std::shared_ptr<MusicPlayList> loadPlayList(DispatchCallback &callback,
                                            const std::vector<std::string> &paths)
{
    static WorkQueue workQueue;
    AudioSpec devSpec;
    devSpec.sampleRate     = 16000;
    devSpec.numChannel     = 1;
    devSpec.bytesPerSample = 2;
    devSpec.bitsPerSample  = 16;
    devSpec.format         = AudioFormatS16;

    std::shared_ptr<MusicPlayList> playlist
        = std::make_shared<MusicPlayList>(&callback, &workQueue, devSpec);
    for (std::vector<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
        std::promise<void> done;
        std::future<void> future = done.get_future();
        playlist->addMusicWithNotify(*it, [&done]() { done.set_value(); });
        // 守卫只防死锁，不是性能判据（不改 ctest TIMEOUT，也不是任何行为断言）。
        // 取值必须**严格大于**被测链路的自身超时预算，否则会把「实现自己正常回退的
        // 超时」记成用例失败：MockHttpServer 单线程 accept，连接上手后 `recv` 超时
        // 2s（setRecvTimeout）；HttpSource 构造期三次探测，每次 socket 读超时 3s。
        // 单连接被阻塞时后续连接排队 → 单次停顿会级联到同一 WorkQueue 上的后续条目。
        // 故 2s（旧值，恰好等于服务端 recv 超时）在 Linux 覆盖构建 + `-j4` 下必然假红。
        const std::future_status status = future.wait_for(std::chrono::seconds(15));
        if (status != std::future_status::ready) {
            FAIL("addMusicWithNotify did not complete within 15s: " << *it);
        }
    }
    return playlist;
}

} // namespace

TEST_SUITE("PlaylistUrlDispatch")
{
    TEST_CASE("playlist accepts local mock URL and matches local file scan")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        DispatchCallback urlCb;
        std::shared_ptr<MusicPlayList> urlList
            = loadPlayList(urlCb, { server.url("/music.wav") });
        REQUIRE(urlList->getMusicCount() == 1);
        REQUIRE(urlCb.currentBufs.size() == 1);
        MusicPropertiesPtr urlProps = urlCb.currentBufs.front();

        DispatchCallback fileCb;
        std::shared_ptr<MusicPlayList> fileList
            = loadPlayList(fileCb, { fixture.mediaPath("music.wav") });
        REQUIRE(fileList->getMusicCount() == 1);
        REQUIRE(fileCb.currentBufs.size() == 1);
        MusicPropertiesPtr fileProps = fileCb.currentBufs.front();

        REQUIRE(urlProps->signalProperties.spec.sampleRate > 0);
        REQUIRE(urlProps->signalProperties.durationMs > 0);
        REQUIRE(urlProps->signalProperties.spec.sampleRate
                == fileProps->signalProperties.spec.sampleRate);
        REQUIRE(urlProps->signalProperties.durationMs
                == fileProps->signalProperties.durationMs);

        uint8_t urlHead[16];
        uint8_t fileHead[16];
        REQUIRE(urlProps->processProperties.source->readAt(0, urlHead, sizeof(urlHead)) == 16);
        REQUIRE(fileProps->processProperties.source->readAt(0, fileHead, sizeof(fileHead)) == 16);
        CHECK(std::memcmp(urlHead, fileHead, sizeof(urlHead)) == 0);
        CHECK(std::memcmp(urlHead, bytes.data(), sizeof(urlHead)) == 0);
    }

    TEST_CASE("playlist accepts a mixed local-file and mock-URL list")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);
        RealMediaFixture fixture;
        REQUIRE(fixture.exists("music.wav"));

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(
            cb, { fixture.mediaPath("music.wav"), server.url("/music.wav") });

        REQUIRE(playlist->getMusicCount() == 2);
        // 沿用既有切歌语义，不为 URL 新造规则
        REQUIRE(playlist->nextSync() == true);
        REQUIRE(playlist->previousSync() == true);
    }

    TEST_CASE("playlist rejects a playlist-class URL with FileOpenFailed")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist
            = loadPlayList(cb, { server.url("/list.m3u8") });

        REQUIRE(playlist->getMusicCount() == 0);
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::FileOpenFailed);
        CHECK(server.requestCount() == 0);   // 构造期明确拒绝，未联网、非静默失败
    }

    TEST_CASE("playlist accepts a URL with query and strips it before extension")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist
            = loadPlayList(cb, { server.url("/music.wav?token=abc") });

        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->fileProperties.extensionName == ".wav");
        REQUIRE(props->fileProperties.fileName == "music");
        REQUIRE(props->signalProperties.spec.sampleRate > 0);
        REQUIRE(props->signalProperties.durationMs > 0);
    }

    TEST_CASE("parseFileName strips query and fragment before the last dot")
    {
        FileProperties withQuery;
        withQuery.fullPath = "http://127.0.0.1:1234/music.wav?token=abc";
        withQuery.parseFileName();
        REQUIRE(withQuery.fileName == "music");
        REQUIRE(withQuery.extensionName == ".wav");

        FileProperties withFragment;
        withFragment.fullPath = "http://127.0.0.1:1234/music.wav#seg";
        withFragment.parseFileName();
        REQUIRE(withFragment.fileName == "music");
        REQUIRE(withFragment.extensionName == ".wav");
    }

    TEST_CASE("parseFileName keeps '#' in local file names")
    {
        FileProperties local;
        local.fullPath = "/tmp/music/song#x.wav";
        local.parseFileName();
        REQUIRE(local.extensionName == ".wav");
        REQUIRE(local.fileName == "song#x");
    }
}

// ---------------------------------------------------------------------------
// P1-1：DataSourceBase 取消信号的内部虚接缝（issue #135）。
// 用最小合成数据源直接判定注入口，不依赖网络 / 素材 / HTTP server。
// ---------------------------------------------------------------------------
namespace {

// 只实现两个纯虚，取消接缝完全沿用基类默认实现。
class AbortSeamSource : public DataSourceBase {
public:
    sdk_utils::status_t initCheck() const override { return sdk_utils::OK; }
    ssize_t readAt(off64_t, void *, size_t) override { return 0; }
};

} // namespace

TEST_SUITE("AbortSignalWiring")
{
    TEST_CASE("abortRequested is false before any injection")
    {
        AbortSeamSource source;
        CHECK(source.abortRequested() == false);
    }

    TEST_CASE("injected flag is observed and follows its value")
    {
        AbortSeamSource source;
        std::atomic<bool> flag(false);
        source.setAbortFlag(&flag);
        CHECK(source.abortRequested() == false);
        flag.store(true);
        CHECK(source.abortRequested() == true);
        flag.store(false);
        CHECK(source.abortRequested() == false);
    }

    TEST_CASE("unbinding the flag returns abortRequested to false")
    {
        AbortSeamSource source;
        std::atomic<bool> flag(true);
        source.setAbortFlag(&flag);
        REQUIRE(source.abortRequested() == true);
        source.setAbortFlag(nullptr);
        CHECK(source.abortRequested() == false);
    }

    // 对称性：同一对象在「注入 → 解绑 → 再注入新 flag」后状态机不卡死，
    // 补 #135 未覆盖的「解绑后重新注入」连续序列；纯本地判定、不联网。
    TEST_CASE("re-injecting a flag after unbinding is still observed")
    {
        AbortSeamSource source;
        std::atomic<bool> first(true);
        source.setAbortFlag(&first);
        REQUIRE(source.abortRequested() == true);    // 置位后取消可见
        source.setAbortFlag(nullptr);
        CHECK(source.abortRequested() == false);     // 解绑后恢复
        std::atomic<bool> second(false);
        source.setAbortFlag(&second);
        CHECK(source.abortRequested() == false);     // 新 flag 初值为假
        second.store(true);
        CHECK(source.abortRequested() == true);      // 新 flag 仍可判定
    }
}

// ---------------------------------------------------------------------------
// P2-1：HLS VOD URL 直通接入（issue #144，契约 #143 resolution）。
// mock 端点最小化：playlist 文本端点 + 其余路径按裸 wav segment 应答（契约 §7 允许
// 「直接裸 wav segment」，hls demuxer 对每个 segment 独立探测封装）。本票只做
// 分派与 open 判据，完整 MockHlsServer 矩阵属 #145。
// ---------------------------------------------------------------------------
namespace {

// 契约 §7 的 media playlist 形态：#EXTM3U + VERSION + TARGETDURATION + N 个
// EXTINF +（VOD 侧）PLAYLIST-TYPE:VOD 与 #EXT-X-ENDLIST。segment 为相对 URI。
std::string makeHlsPlaylist(bool vod)
{
    std::string text =
        "#EXTM3U\r\n"
        "#EXT-X-VERSION:3\r\n"
        "#EXT-X-TARGETDURATION:10\r\n";
    if (vod) {
        text += "#EXT-X-PLAYLIST-TYPE:VOD\r\n";
    }
    text += "#EXTINF:10.0,\r\nseg0.ts\r\n"
            "#EXTINF:10.0,\r\nseg1.ts\r\n";
    if (vod) {
        text += "#EXT-X-ENDLIST\r\n";
    }
    return text;
}

} // namespace

TEST_SUITE("HlsUrlDirect")
{
    TEST_CASE("VOD playlist URL scans via the direct path with positive duration")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/vod";
        cfg.playlistText = makeHlsPlaylist(true);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/vod") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs > 0);
        CHECK(props->signalProperties.spec.sampleRate > 0);
    }

    TEST_CASE("createExtractor opens a UrlSource against a VOD playlist URL")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/vod";
        cfg.playlistText = makeHlsPlaylist(true);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        UrlSource source(server.url("/vod").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        CHECK(source.headLooksLikePlaylist() == true);
        CHECK(source.headHasVodFeatures() == true);
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, "m3u8", true));
        REQUIRE(extractor != nullptr);
        CHECK(extractor->getAudioSpec().durationMs > 0);
        CHECK(extractor->getDataSource() == &source);
    }

    TEST_CASE("LIVE playlist URL is rejected without silent fallback")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/live";
        cfg.playlistText = makeHlsPlaylist(false);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live") });
        REQUIRE(playlist->getMusicCount() == 0);
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::FileOpenFailed);
        CHECK(server.requestCount() >= 1);   // 已做头部嗅探：拒绝是明确的分派失败，非静默
    }

    TEST_CASE(".mpd URL keeps the P0 networkless rejection")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer server(bytes);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist
            = loadPlayList(cb, { server.url("/manifest.mpd") });
        REQUIRE(playlist->getMusicCount() == 0);
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::FileOpenFailed);
        CHECK(server.requestCount() == 0);   // 扩展名层维持 P0：明确拒绝且不联网
    }

    TEST_CASE("UrlSource abort seam observes the injected flag")
    {
        // 端口 9（discard）连接即拒：构造期嗅探必失败，但取消接缝与 initCheck 解耦，
        // 与 HttpSource 的接缝语义完全同构（#143 契约 §4）。
        UrlSource source("http://127.0.0.1:9/p2-1-nope");
        std::atomic<bool> flag(false);
        source.setAbortFlag(&flag);
        CHECK(source.abortRequested() == false);
        flag.store(true);
        CHECK(source.abortRequested() == true);
    }
}

// ---------------------------------------------------------------------------
// P1-2：avioRead 取消检查与 interrupt_callback 共用的两个只读检查点（issue #136）。
// 直接 include 共享头，验证同一取消信号在两条落点的取值，不联网、不起 server。
// ---------------------------------------------------------------------------
TEST_SUITE("AbortInterruptContract")
{
    TEST_CASE("no injection yields false / 0 for both checkpoints")
    {
        AbortSeamSource source;
        CHECK(sbAbortRequested(&source) == false);
        CHECK(sbAbortInterruptCallback(&source) == 0);
        CHECK(sbAbortInterruptCallback(nullptr) == 0);
    }

    TEST_CASE("injected flag is observed by both checkpoints and follows its value")
    {
        AbortSeamSource source;
        std::atomic<bool> flag(false);
        source.setAbortFlag(&flag);
        CHECK(sbAbortRequested(&source) == false);
        CHECK(sbAbortInterruptCallback(&source) == 0);
        flag.store(true);
        CHECK(sbAbortRequested(&source) == true);
        CHECK(sbAbortInterruptCallback(&source) == 1);
        flag.store(false);
        CHECK(sbAbortRequested(&source) == false);
        CHECK(sbAbortInterruptCallback(&source) == 0);
    }

    TEST_CASE("unbinding the flag returns both checkpoints to false / 0")
    {
        AbortSeamSource source;
        std::atomic<bool> flag(true);
        source.setAbortFlag(&flag);
        REQUIRE(sbAbortRequested(&source) == true);
        source.setAbortFlag(nullptr);
        CHECK(sbAbortRequested(&source) == false);
        CHECK(sbAbortInterruptCallback(&source) == 0);
    }
}
