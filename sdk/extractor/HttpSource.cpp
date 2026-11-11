#include "HttpSource.h"
#include "LogWrapper.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define LOG_TAG "HttpSource"

using namespace sdk_utils;

namespace {

// Winsock 进程级引用计数：首次启动、末次清理，避免与本地 mock server
// 的初始化互相干扰（Windows 自身的 WSAStartup 也是引用计数的）。
std::mutex &wsaMutex()
{
    static std::mutex m;
    return m;
}

int &wsaRefCount()
{
    static int n = 0;
    return n;
}

bool wsaAcquire()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(wsaMutex());
    if (wsaRefCount() == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return false;
        }
    }
    ++wsaRefCount();
#endif
    return true;
}

void wsaRelease()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(wsaMutex());
    --wsaRefCount();
    if (wsaRefCount() <= 0) {
        wsaRefCount() = 0;
        WSACleanup();
    }
#endif
}

#ifdef _WIN32
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kInvalidSocket = -1;
inline void closeSocket(Socket s) { ::close(s); }
#endif

// socket 层读/写超时：只防测试挂死，不是 P1 播放任务级取消/超时。
void setSocketTimeout(Socket s)
{
#ifdef _WIN32
    DWORD ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAll(Socket s, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s, data + sent, (int)(len - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

struct Url {
    std::string host;
    std::string port;
    std::string path;   // 含 query，已去 fragment
};

bool parseUrl(const std::string &url, Url &out)
{
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    const size_t rest = scheme.size();
    const size_t slash = url.find('/', rest);
    const std::string authority = (slash == std::string::npos)
        ? url.substr(rest) : url.substr(rest, slash - rest);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
    const size_t frag = path.find('#');
    if (frag != std::string::npos) path = path.substr(0, frag);
    if (path.empty()) path = "/";
    std::string host = authority;
    std::string port = "80";
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (port.empty()) port = "80";
    }
    if (host.empty()) return false;
    out.host = host;
    out.port = port;
    out.path = path;
    return true;
}

bool isPlaylistExtension(const std::string &path)
{
    std::string p = path;
    const size_t frag = p.find('#');
    if (frag != std::string::npos) p = p.substr(0, frag);
    const size_t q = p.find('?');
    if (q != std::string::npos) p = p.substr(0, q);
    const size_t slash = p.rfind('/');
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos) return false;
    if (slash != std::string::npos && dot < slash) return false;
    std::string ext;
    for (size_t i = dot; i < p.size(); ++i) {
        ext += (char)std::tolower((unsigned char)p[i]);
    }
    return ext == ".m3u8" || ext == ".mpd";
}

std::string trim(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

std::string lowerCopy(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        r += (char)std::tolower((unsigned char)s[i]);
    }
    return r;
}

bool parseInt64(const std::string &s, off64_t &out)
{
    if (s.empty()) return false;
    off64_t v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

struct Response {
    int status;
    std::vector<std::string> headers;
    std::vector<uint8_t> body;
    Response() : status(0) { }
};

std::string headerValue(const std::vector<std::string> &headers, const std::string &name)
{
    const std::string lowerName = lowerCopy(name);
    for (size_t i = 0; i < headers.size(); ++i) {
        const size_t colon = headers[i].find(':');
        if (colon == std::string::npos) continue;
        if (lowerCopy(trim(headers[i].substr(0, colon))) == lowerName) {
            return trim(headers[i].substr(colon + 1));
        }
    }
    return std::string();
}

bool hasIcyHeader(const std::vector<std::string> &headers)
{
    for (size_t i = 0; i < headers.size(); ++i) {
        const size_t colon = headers[i].find(':');
        const std::string key = lowerCopy(trim(headers[i].substr(
            0, (colon == std::string::npos) ? headers[i].size() : colon)));
        if (key.compare(0, 4, "icy-") == 0) return true;
    }
    return false;
}

bool hasAcceptRangesBytes(const std::vector<std::string> &headers)
{
    return lowerCopy(headerValue(headers, "Accept-Ranges")).find("bytes")
        != std::string::npos;
}

bool parseTotal(const std::vector<std::string> &headers, off64_t &total)
{
    const std::string cr = headerValue(headers, "Content-Range");
    const size_t slash = cr.rfind('/');
    if (slash == std::string::npos) return false;
    return parseInt64(trim(cr.substr(slash + 1)), total);
}

void parseHead(const std::string &head, Response &out)
{
    size_t pos = 0;
    bool first = true;
    while (pos < head.size()) {
        const size_t eol = head.find("\r\n", pos);
        const size_t end = (eol == std::string::npos) ? head.size() : eol;
        const std::string line = head.substr(pos, end - pos);
        pos = (eol == std::string::npos) ? head.size() : eol + 2;
        if (first) {
            first = false;
            const size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                const size_t sp2 = line.find(' ', sp + 1);
                const std::string code = line.substr(
                    sp + 1, (sp2 == std::string::npos) ? std::string::npos : sp2 - sp - 1);
                out.status = std::atoi(code.c_str());
            }
        } else {
            out.headers.push_back(line);
        }
    }
}

bool doRequest(const std::string &host, const std::string &port,
               const std::string &path, off64_t start, off64_t end,
               Response &out)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return false;

    Socket sock = kInvalidSocket;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidSocket) continue;
        setSocketTimeout(sock);
        if (::connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closeSocket(sock);
        sock = kInvalidSocket;
    }
    freeaddrinfo(result);
    if (sock == kInvalidSocket) return false;

    std::string hostHeader = host;
    if (port != "80") hostHeader += ":" + port;
    const std::string req = "GET " + path + " HTTP/1.1\r\n"
                          + "Host: " + hostHeader + "\r\n"
                          + "Range: bytes=" + std::to_string((long long)start) + "-"
                          + std::to_string((long long)end) + "\r\n"
                          + "Connection: close\r\n"
                          + "\r\n";
    if (!sendAll(sock, req.data(), req.size())) {
        closeSocket(sock);
        return false;
    }

    std::string raw;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = raw.find("\r\n\r\n")) == std::string::npos) {
        const int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            closeSocket(sock);
            return false;
        }
        raw.append(buf, (size_t)n);
        if (raw.size() > 256 * 1024) {
            closeSocket(sock);
            return false;
        }
    }
    parseHead(raw.substr(0, headerEnd), out);

    off64_t contentLength = -1;
    const std::string cl = headerValue(out.headers, "Content-Length");
    off64_t clParsed = 0;
    if (!cl.empty() && parseInt64(cl, clParsed)) contentLength = clParsed;

    const std::string rest = raw.substr(headerEnd + 4);
    out.body.assign(rest.begin(), rest.end());
    if (contentLength >= 0) {
        while ((off64_t)out.body.size() < contentLength) {
            const int n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.body.insert(out.body.end(), buf, buf + n);
        }
    } else {
        while (true) {
            const int n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.body.insert(out.body.end(), buf, buf + n);
        }
    }
    closeSocket(sock);
    if (contentLength >= 0 && (off64_t)out.body.size() != contentLength) return false;
    return true;
}

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 307 || status == 308;
}

// Location 可能是绝对 URL 或相对路径；P0 只接 http，简单处理同 host 拼接。
bool resolveLocation(const std::string &loc,
                     const std::string &curHost, const std::string &curPort,
                     const std::string &curPath,
                     std::string &host, std::string &port, std::string &path)
{
    if (loc.empty()) return false;
    if (loc.compare(0, 7, "http://") == 0) {
        Url u;
        if (!parseUrl(loc, u)) return false;
        host = u.host;
        port = u.port;
        path = u.path;
        return true;
    }
    if (loc.compare(0, 8, "https://") == 0) return false;
    host = curHost;
    port = curPort;
    if (loc[0] == '/') {
        path = loc;
    } else {
        const size_t slash = curPath.rfind('/');
        const std::string dir = (slash == std::string::npos)
            ? std::string("/") : curPath.substr(0, slash + 1);
        path = dir + loc;
    }
    const size_t frag = path.find('#');
    if (frag != std::string::npos) path = path.substr(0, frag);
    if (path.empty()) path = "/";
    return true;
}

// 伪长度二次校验：再取最后一个字节，要求 206、total 一致、body 恰好 1 字节。
bool verifyTail(const std::string &host, const std::string &port,
                const std::string &path, off64_t total)
{
    Response resp;
    if (!doRequest(host, port, path, total - 1, total - 1, resp)) return false;
    if (hasIcyHeader(resp.headers) || resp.status != 206) return false;
    off64_t t = 0;
    if (!parseTotal(resp.headers, t) || t != total) return false;
    if (resp.body.size() != 1) return false;
    return true;
}

} // namespace

HttpSource::HttpSource(const char *url)
    : m_total(-1), m_initCheck(NO_INIT), m_wsaHeld(false)
{
    m_name = url ? std::string(url) : std::string("<null>");
    LOG_DEBUG(LOG_TAG, "%s", m_name.c_str());

    if (url == nullptr || url[0] == '\0') {
        LOG_ERROR(LOG_TAG, "empty url");
        return;
    }
    const std::string u(url);
    if (u.compare(0, 8, "https://") == 0) {
        LOG_ERROR(LOG_TAG, "https is not supported in P0: %s", u.c_str());
        return;   // 明确拒绝且不联网
    }
    Url parsed;
    if (!parseUrl(u, parsed)) {
        LOG_ERROR(LOG_TAG, "invalid http url: %s", u.c_str());
        return;
    }
    if (isPlaylistExtension(parsed.path)) {
        LOG_ERROR(LOG_TAG, "playlist source rejected in P0: %s", u.c_str());
        return;   // 明确拒绝且不联网
    }
    if (!wsaAcquire()) {
        LOG_ERROR(LOG_TAG, "WSAStartup failed");
        return;
    }
    m_wsaHeld = true;
    m_host = parsed.host;
    m_port = parsed.port;
    m_path = parsed.path;
    if (!probe()) {
        LOG_ERROR(LOG_TAG, "probe failed: %s", u.c_str());
        return;
    }
    m_initCheck = OK;
    LOGI("http source ready, total=%lld", (long long)m_total);
}

HttpSource::~HttpSource()
{
    if (m_wsaHeld) {
        wsaRelease();
        m_wsaHeld = false;
    }
}

sdk_utils::status_t HttpSource::initCheck() const
{
    return m_initCheck;
}

ssize_t HttpSource::readAt(off64_t offset, void *data, size_t size)
{
    if (m_initCheck != OK) return -1;
    std::lock_guard<std::mutex> lock(m_lock);
    if (offset >= m_total) return 0;
    const off64_t available = m_total - offset;
    if ((off64_t)size > available) size = (size_t)available;
    if (size == 0 || data == nullptr) return 0;

    Response resp;
    if (!doRequest(m_host, m_port, m_path, offset, offset + (off64_t)size - 1, resp)) {
        LOG_ERROR(LOG_TAG, "readAt request failed at %lld", (long long)offset);
        return -1;
    }
    if (hasIcyHeader(resp.headers) || resp.status != 206) return -1;
    off64_t total = 0;
    if (!parseTotal(resp.headers, total) || total != m_total) return -1;
    if (resp.body.size() != size) return -1;
    std::memcpy(data, resp.body.data(), size);
    return (ssize_t)size;
}

sdk_utils::status_t HttpSource::getSize(off64_t *size)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_initCheck != OK) return NO_INIT;
    *size = m_total;
    return OK;
}

void HttpSource::close()
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_initCheck = NO_INIT;
}

bool HttpSource::probe()
{
    // 探测：Range: bytes=0-0，要求 206 + Accept-Ranges: bytes + 可解析总长；
    // 301/302/307/308 + Location 最多 5 次重定向后重新探测，不改形态归类。
    for (int redirects = 0; redirects <= 5; ++redirects) {
        Response resp;
        if (!doRequest(m_host, m_port, m_path, 0, 0, resp)) return false;
        if (hasIcyHeader(resp.headers)) {
            LOG_ERROR(LOG_TAG, "Icy-* header present, reject infinite stream");
            return false;
        }
        if (isRedirect(resp.status)) {
            if (redirects == 5) {
                LOG_ERROR(LOG_TAG, "too many redirects");
                return false;
            }
            std::string host, port, path;
            if (!resolveLocation(headerValue(resp.headers, "Location"),
                                 m_host, m_port, m_path, host, port, path)) {
                return false;
            }
            m_host = host;
            m_port = port;
            m_path = path;
            // 重定向更新 m_path 后复查清单类，明确拒绝，避免 P0-1 的漏检。
            if (isPlaylistExtension(m_path)) {
                LOG_ERROR(LOG_TAG, "redirect target is a playlist, reject: %s",
                          m_path.c_str());
                return false;
            }
            continue;
        }
        if (resp.status != 206) {
            LOG_ERROR(LOG_TAG, "probe expects 206, got %d", resp.status);
            return false;
        }
        if (!hasAcceptRangesBytes(resp.headers)) {
            LOG_ERROR(LOG_TAG, "Accept-Ranges: bytes missing");
            return false;
        }
        off64_t total = 0;
        if (!parseTotal(resp.headers, total) || total <= 0) {
            LOG_ERROR(LOG_TAG, "invalid Content-Range total");
            return false;
        }
        if (!verifyTail(m_host, m_port, m_path, total)) {
            LOG_ERROR(LOG_TAG, "pseudo-length second check failed");
            return false;
        }
        m_total = total;
        return true;
    }
    return false;
}
