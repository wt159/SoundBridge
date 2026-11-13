#include "../fixtures/RealMediaFixture.hpp"
#include "../fixtures/TestProcess.hpp"
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <ExtractorFactory.h>
#include <HttpSource.h>
#include <MusicPlayList.h>
#include <UrlSource.h>
#include <doctest/doctest.h>
#include "AbortInterrupt.hpp"
// P3 共享 mock 在所有 sdk 头之后 include：HttpMocks 自带 winsock2，
// wingdi 的 ERROR 宏会污染 StreamDecoderState::ERROR（保持原文件的展开顺序）。
#include "../mocks/HttpMocks.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// P3：HTTP mock 抽到共享头 sdk/test/mocks/HttpMocks.hpp（LiveSmoke 同源复用），
// 本文件沿用原匿名 namespace 的可见性语义。
using namespace sb_httpmock;


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
        // P2 判据演进（#144 review，规格 §9.3 S2 重开）：.m3u8 URL 先经 UrlSource
        // 头部嗅探联网确认——本端点内容非清单（素材字节）→ 回落 P0 扩展名层拒绝，
        // 拒绝仍是明确的 FileOpenFailed，非静默。恰 1 次嗅探请求。
        CHECK(server.requestCount() == 1);
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

// P2-2（issue #145）：PLAYLIST-TYPE:EVENT（有界但增长，无 ENDLIST）。
// 依 #143 契约 §2 归 LIVE 侧拒绝。
std::string makeEventPlaylist()
{
    return "#EXTM3U\r\n"
           "#EXT-X-VERSION:3\r\n"
           "#EXT-X-TARGETDURATION:10\r\n"
           "#EXT-X-PLAYLIST-TYPE:EVENT\r\n"
           "#EXTINF:10.0,\r\nseg0.ts\r\n"
           "#EXTINF:10.0,\r\nseg1.ts\r\n";
}

// P2-2：master playlist（#EXT-X-STREAM-INF + 相对 variant URI），无 ENDLIST——
// VOD/LIVE 语义由 variant 承载（分派嗅探跟随首个 variant 复查，见 UrlSource）。
std::string makeMasterPlaylist()
{
    return "#EXTM3U\r\n"
           "#EXT-X-VERSION:3\r\n"
           "#EXT-X-STREAM-INF:BANDWIDTH=128000\r\n"
           "v1.m3u8\r\n";
}

// P2-2：playlist 内 segment URI 写 file:/// 绝对本地路径（指向确知存在的素材，
// 反向证实「白名单拒绝」而非「路径打不开」）。形态仍是 VOD 清单。
std::string makeFileEscapePlaylist(const std::string &fileUrl)
{
    return "#EXTM3U\r\n"
           "#EXT-X-VERSION:3\r\n"
           "#EXT-X-TARGETDURATION:10\r\n"
           "#EXT-X-PLAYLIST-TYPE:VOD\r\n"
           "#EXTINF:10.0,\r\n" + fileUrl + "\r\n"
           "#EXTINF:10.0,\r\n" + fileUrl + "\r\n"
           "#EXT-X-ENDLIST\r\n";
}

// 本地路径 → file: URL（确知存在的素材路径；若白名单失效，hls demuxer 能
// 真把它打开，从而该判据的变异注入「白名单含 file」必红，排除假绿）。
// Windows 上本 FFmpeg 构建只认 "file:D:/..." 形态（file:/// 与 file:// 均
// 返回 ENOENT，已实测），故不带斜杠前缀。
std::string fileUrlFromPath(const std::string &path)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    if (_fullpath(buf, path.c_str(), MAX_PATH) == nullptr) return std::string();
    std::string out(buf);
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\') out[i] = '/';
    }
    return "file:" + out;
#else
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) == nullptr) return std::string();
    return std::string("file://") + buf;
#endif
}

// —— P2-2 解码侧观察 helper（形态同 test_stream_decoder.cpp 的同名工具）——

// 本 TU 经 winsock 头链拉入了 windows.h，其 ERROR 宏与 StreamDecoderState 的
// ERROR 成员撞名：作用域内取消宏定义，取别名后立即恢复，不影响其余代码。
#pragma push_macro("ERROR")
#undef ERROR
namespace {
const StreamDecoderState kDecoderErrorState = StreamDecoderState::ERROR;
}
#pragma pop_macro("ERROR")

// 边排空 ring 边等目标状态（EOS 需要消费端持续排空，否则背压停在线程里）。
// 守卫取值必须远大于被测链路预算（#130）：直通 open（嗅探 + playlist +
// segment）秒级，解码 50s PCM 亚秒级，给 30s。
bool waitForDecoderStateWithDrain(AudioStreamDecoder &decoder, AudioRingBuffer &ring,
                                  StreamDecoderState expectedState, int timeoutMs)
{
    const int pollMs = 5;
    int elapsed      = 0;
    std::vector<char> drain(4096);
    while (elapsed <= timeoutMs) {
        while (ring.availableRead() > 0) {
            const size_t once = std::min(drain.size(), ring.availableRead());
            if (ring.read(drain.data(), once) == 0) {
                break;
            }
        }
        const StreamDecoderState state = decoder.state();
        if (state == expectedState) {
            return true;
        }
        if (state == kDecoderErrorState) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        elapsed += pollMs;
    }
    return false;
}

// 按字节数排空 ring（等待生产者补齐，守卫 10s ≫ 链路预算）。
bool drainRingBytes(AudioRingBuffer &ring, size_t bytes)
{
    size_t got = 0;
    std::vector<char> buf(4096);
    int waitedMs = 0;
    while (got < bytes && waitedMs <= 10000) {
        const size_t avail = ring.availableRead();
        if (avail > 0) {
            const size_t once = std::min(std::min(avail, bytes - got), buf.size());
            const size_t n    = ring.read(buf.data(), once);
            if (n == 0) return false;
            got += n;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            waitedMs += 2;
        }
    }
    return got == bytes;
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
        cfg.hlsContinuousStream = true;   // P2-2 连续流素材：多 segment 跨段供数
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

    // P3 判据演进（#152 D17，票 #159）：LIVE 正面特征集（#EXTM3U + 无 ENDLIST
    // + 无 PLAYLIST-TYPE:VOD + 有分片行）→ 直通放行，durationMs==0 是 LIVE 的
    // 单一信号（无限流无时长）；断流重连等播放级判据由独立可执行 LiveSmoke 承接
    // （规格 §10.2），ctest 只收「分派放行 + 扫描快」。
    TEST_CASE("LIVE playlist URL is admitted to the direct path with durationMs 0")
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
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        CHECK(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);   // LIVE 单一信号
        CHECK(props->signalProperties.spec.sampleRate > 0);
        CHECK(server.requestCount() >= 1);   // 头部嗅探 + 直通 open 都在本地 mock 上
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

    // P2 判据演进（#144 review，规格 §9.3 S2 重开）：.m3u8 是 HLS VOD 最主要入口，
    // 分派对 .m3u8 URL 同样做头部嗅探 + VOD 特征区分，不再一律走 P0 扩展名层拒绝。
    TEST_CASE(".m3u8 VOD URL is dispatched to the direct path")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/vod.m3u8";
        cfg.playlistText = makeHlsPlaylist(true);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/vod.m3u8") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs > 0);
        CHECK(props->signalProperties.spec.sampleRate > 0);
    }

    // P3 判据演进（#152 D17）：.m3u8 LIVE 与无扩展名 LIVE 同判——嗅探只看内容
    // 形态，不看 URL 扩展名；放行走同一条直通路径。
    TEST_CASE(".m3u8 LIVE URL is dispatched to the direct path")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/live.m3u8";
        cfg.playlistText = makeHlsPlaylist(false);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live.m3u8") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        CHECK(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);
        CHECK(props->signalProperties.spec.sampleRate > 0);
        CHECK(server.requestCount() >= 1);
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

    // —— P2-2 门禁判据矩阵（issue #145，#143 契约 §7；P3 由 #152/#159 演进：
    // EVENT 归 LIVE 放行（D18）、伪清单与 variant 取回失败维持明确拒绝）——

    // P3 判据演进（#152 D18）：PLAYLIST-TYPE:EVENT（有界但增长、无 ENDLIST）
    // 归 LIVE 放行，语义与裸 LIVE 完全一致（durationMs==0 单一信号）。
    TEST_CASE("EVENT playlist is admitted like LIVE")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/event";
        cfg.playlistText = makeEventPlaylist();
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/event") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        CHECK(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);
        CHECK(props->signalProperties.spec.sampleRate > 0);
    }

    // P3（票 #159）：伪清单（#EXTM3U 起始但不满足 VOD 或 LIVE 正面特征集）
    // 维持明确拒绝不回退——防止空清单 / 坏清单被放进来再无限重试（规格 §9.1）。
    TEST_CASE("pseudo playlist with no segment lines is still rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/pseudo";
        cfg.playlistText =
            "#EXTM3U\r\n"
            "#EXT-X-VERSION:3\r\n"
            "#EXT-X-TARGETDURATION:10\r\n";   // 无 ENDLIST / VOD、无 EXTINF / STREAM-INF
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/pseudo") });
        REQUIRE(playlist->getMusicCount() == 0);
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::FileOpenFailed);
        CHECK(server.requestCount() >= 1);   // 嗅探确认过清单形态，拒绝在分派层
    }

    // P3（票 #159）：master 的 VOD/LIVE 语义由 variant 承载；variant 取回失败
    // 明确拒绝（m_headLive=false → 分派失败出口），不静默回退、不保守归 LIVE。
    TEST_CASE("master playlist with a failed variant fetch is rejected")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistMap["/master.m3u8"] = makeMasterPlaylist();
        cfg.failFromRequest = 1;   // 请求 0 = master 取回成功；请求 1 = variant → 404
        cfg.failRequests   = 1;
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist
            = loadPlayList(cb, { server.url("/master.m3u8") });
        REQUIRE(playlist->getMusicCount() == 0);
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::FileOpenFailed);
        CHECK(server.requestCount() == 2);   // 恰好 master + variant 两次嗅探
    }

    // P3（#152 D19，票 #159）：ICY 无限直播流 → HttpSource 探测命中 Icy-* 头
    // → 明确拒绝自定义 AVIO 路径、改走原生 URL 层直通（复用同一 HlsExtractor，
    // demuxer 自动探测）。mock 只在客户端请求带 Icy-MetaData: 1 才回 Icy-* 头：
    // 放行本身即证明 HttpSource 的补头存在（去掉必红）。
    TEST_CASE("ICY stream URL is dispatched to the native URL path")
    {
        REQUIRE(!mp3FixtureBytes().empty());   // 素材缺失要显式红，不许静默假绿
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.icyStreamPath = "/icy";
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/icy") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        CHECK(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);   // 无限流：无时长单一信号
        CHECK(props->signalProperties.spec.sampleRate > 0);
    }

    // P3（#151 D11/D13，票 #159）：诊断旁路三条生命周期不变量（VOD 端点 +
    // segment 打不开的失败面）。不变量 1：open 成功后无失败可消费；不变量 2：
    // 成功读包期间不得残留失败状态；不变量 3：失败记入一次、消费一次即失效、
    // 持续失败可重复置位。FFmpeg 4.4 事实基线（#148 §2）：segment 404 → 跳过，
    // VOD 收场于 AVERROR_EOF——诊断把「EOF/失败」统一记入，分流在消费侧。
    TEST_CASE("diagnostic side channel observes its lifecycle invariants")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/diag";
        cfg.playlistText = makeHlsPlaylist(true);
        // 请求 0 = 本测 UrlSource 自身的嗅探、1 = 提取器 playlist 取回、
        // 2 = seg0 打开；此后全部 404：seg0 读尽后 seg1 打不开 → VOD 以
        // EOF 收场（研究文档 §2 的 404 跳过事实）。
        cfg.failFromRequest = 3;
        cfg.failRequests   = 1000000;
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        UrlSource source(server.url("/diag").c_str());
        REQUIRE(source.initCheck() == sdk_utils::OK);
        std::unique_ptr<ExtractorHelper> extractor(
            ExtractorFactory::createExtractor(&source, "m3u8", true));
        REQUIRE(extractor != nullptr);

        int err = -1;
        // 不变量 1：open 成功后无失败可消费（且无陈旧残留）。
        CHECK(extractor->consumeLastReadError(&err) == false);
        CHECK(err == 0);

        // 顺序读包至首个 nullptr：成功包期间诊断不得置位（不变量 2 的可观测形式）。
        const size_t kMaxPackets = 100000;
        size_t consumed = 0;
        while (consumed < kMaxPackets) {
            if (extractor->packetAt(consumed) == nullptr) break;
            ++consumed;
            CHECK(extractor->consumeLastReadError(&err) == false);
        }
        REQUIRE(consumed < kMaxPackets);   // seg0 有包、seg1 打不开收场

        // 不变量 3 前半：失败被记入且首次消费取到非零错误码。
        REQUIRE(extractor->consumeLastReadError(&err) == true);
        CHECK(err != 0);
        // 不变量 3 后半：同一失败不得被消费两次。
        CHECK(extractor->consumeLastReadError(&err) == false);

        // 持续失败面：再读仍 nullptr，诊断重新置位、可再次消费。
        REQUIRE(extractor->packetAt(consumed) == nullptr);
        REQUIRE(extractor->consumeLastReadError(&err) == true);
        CHECK(err != 0);
    }

    TEST_CASE("file:// segment escape is rejected by the protocol whitelist")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        // segment URL 指向确知存在的本地素材：若白名单失效（变异注入 file），
        // hls demuxer 能真打开它 → 判据必红，排除「路径打不开」造成的假绿。
        const std::string fileUrl = fileUrlFromPath(
            RealMediaFixture().mediaPath("music.wav"));
        REQUIRE(!fileUrl.empty());

        MockHttpServer::Config cfg;
        cfg.playlistPath = "/escape";
        cfg.playlistText = makeFileEscapePlaylist(fileUrl);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/escape") });
        REQUIRE(playlist->getMusicCount() == 0);
        // 直通 open 失败（数据源层）走 createExtractor 出口：扩展名已注册 →
        // ExtractorInitFailed（#143 契约 §6 错误分层），分派嗅探已确认 VOD 形态。
        REQUIRE(cb.errors.size() == 1);
        REQUIRE(cb.errors.front() == ErrorCode::ExtractorInitFailed);
        CHECK(server.requestCount() >= 1);   // 清单本身取回过，拒绝发生在白名单外的 segment
    }

    TEST_CASE("VOD media playlist scans and decodes through to EOS")
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

        // 解码到 EOS：PCM 逐包供给分支（codec NONE + 包工厂），消费端持续排空。
        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);
        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 30000));
        decoder.stop();
    }

    TEST_CASE("master playlist resolves a variant and decodes through to EOS")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistMap["/master.m3u8"] = makeMasterPlaylist();
        cfg.hlsContinuousStream = true;   // P2-2 连续流素材：多 segment 跨段供数
        cfg.playlistMap["/v1.m3u8"]     = makeHlsPlaylist(true);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist
            = loadPlayList(cb, { server.url("/master.m3u8") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs > 0);
        CHECK(props->signalProperties.spec.sampleRate > 0);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);
        REQUIRE(waitForDecoderStateWithDrain(decoder, ring, StreamDecoderState::EOS, 30000));
        decoder.stop();
    }

    TEST_CASE("seek on a VOD stream lands on the requested position")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.playlistPath = "/vod";
        cfg.hlsContinuousStream = true;   // P2-2 连续流素材：多 segment 跨段供数
        cfg.playlistText = makeHlsPlaylist(true);
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/vod") });
        REQUIRE(playlist->getMusicCount() == 1);
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs > 0);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);

        // 消费 500ms：位置按消费字节单调推进（16000Hz 单声道 16bit = 32 B/ms）。
        REQUIRE(drainRingBytes(ring, 500 * 32));
        REQUIRE(decoder.positionMs() == 500);

        // 顺序重放档 seek：段基准先落地（位置从 500 跳到目标，不掉回 0），
        // 目标之前的 PCM 由写入口逐字节丢弃；落在目标点后位置精确等于 2000。
        decoder.seekToMs(2000);
        bool landed = false;
        for (int waited = 0; waited <= 10000 && !landed; waited += 2) {
            if (waited % 500 == 0) {
                std::printf("[probe] t=%dms pos=%llu state=%d ringAvail=%zu\n",
                            waited, (unsigned long long)decoder.positionMs(),
                            (int)decoder.state(), ring.availableRead());
            }
            if (decoder.positionMs() == 2000) {
                landed = true;
            } else if (decoder.state() == kDecoderErrorState) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        std::printf("[probe] final pos=%llu state=%d\n",
                    (unsigned long long)decoder.positionMs(), (int)decoder.state());
        CHECK(decoder.state() != kDecoderErrorState);
        CHECK(landed);
        // 从目标点继续消费，位置从 2000 往前推（8000 字节 = 250ms）。
        REQUIRE(drainRingBytes(ring, 8000));
        CHECK(decoder.positionMs() == 2250);

        decoder.stop();
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
