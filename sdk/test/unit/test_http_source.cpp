#include "../fixtures/RealMediaFixture.hpp"
#include "../fixtures/TestProcess.hpp"
#include <HttpSource.h>
#include <doctest/doctest.h>
#include <atomic>
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
        bool fakeTotalOnTail;
        int  redirects;
        Config()
            : emitIcy(false), omitAcceptRanges(false),
              fakeTotalOnTail(false), redirects(0) { }
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
            const std::string resp = "HTTP/1.1 302 Found\r\n"
                                     "Location: /final.wav\r\n"
                                     "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                     "Connection: close\r\n\r\n" + body;
            sendAll(client, resp);
            return;
        }
        long long start = 0, end = 0;
        const bool hasRange = parseRange(req, start, end);
        const long long total = (long long)m_bytes.size();
        if (!hasRange) {
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

        long long declaredTotal = total;
        if (m_config.fakeTotalOnTail && length == 1 && start == total - 1) {
            declaredTotal = total + 7;   // 伪长度：二次校验 total 与首次不一致
        }
        std::string resp = "HTTP/1.1 206 Partial Content\r\n";
        if (!m_config.omitAcceptRanges) resp += "Accept-Ranges: bytes\r\n";
        resp += "Content-Range: bytes " + std::to_string(start) + "-"
                + std::to_string(end) + "/" + std::to_string(declaredTotal) + "\r\n";
        resp += "Content-Length: " + std::to_string(length) + "\r\n";
        resp += "Connection: close\r\n";
        resp += "X-Mock-Owner: " + std::to_string(sb_test::processId()) + "\r\n";
        if (m_config.emitIcy) resp += "Icy-Name: mock-radio\r\n";
        resp += "\r\n";
        if (length > 0) {
            resp.append((const char *)m_bytes.data() + start, (size_t)length);
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
    Config m_config;
    Socket m_listen;
    int m_port;
    std::atomic<bool> m_stop;
    std::atomic<int> m_requests;
    std::thread m_thread;
};

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
}
