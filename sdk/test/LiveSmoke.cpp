// P3-2 手工冒烟（票 #159 / doc/live-source-spec.md §10.2 D23）：不注册 ctest 的
// 独立可执行，由人在本机跑（三档归类 D24：HLS LIVE / ICY 仅手工冒烟）。
// 判据 = 事件计数 + 回调序列 + 「取消后有限时间内返回」；墙钟只作场景时长与
// 防挂死守卫，不基于墙钟数值断言。素材全部本地 mock、进程私有端口、公网零依赖。
//
// 场景（对应 §10.1 ②③ 与 #150/#153 的正路径契约）：
//   1) windowed LIVE 稳态：播放 8s 不主动结束（零 EOS / 零 ERROR / 零重连），
//      position 单调推进、duration 恒 0；
//   2) ICY 稳态：原生 URL 路径播放 8s 不主动结束、position 单调推进；
//   3) stop() 有限时间返回：稳态播放中 stop()，std::future ≤5s 内就绪
//      （stop 是三条终止路径之一 D20；100ms 取消粒度之上 5s 是防挂死守卫）。
// 每个场景结束输出事件计数（重连尝试 / mock 请求数 / 404 数）。
//
// include 顺序纪律与 test_http_source.cpp / test_live_source.cpp 一致：sdk 头在
// HttpMocks.hpp 之前——HttpMocks 自带 winsock2，wingdi 的 ERROR 宏会污染
// StreamDecoderState 的 ERROR 枚举成员。
#include "../fixtures/RealMediaFixture.hpp"
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <LogWrapper.h>
#include <MusicPlayList.h>
#include <TestProcess.hpp>
// P3 共享 mock 在所有 sdk 头之后 include（见文件头注释）。
#include "../mocks/HttpMocks.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sb_httpmock;
using namespace sdk;

namespace {

int g_failures = 0;

// SDK 全局日志必须先于任何 MusicPlayList / 解码器使用初始化（UnitTestsMain.cpp
// 同构）：MusicPlayList 构造函数就会走全局 LogWrapper，未初始化时 this=0x0 直接段崩溃。
struct LogInit {
    LogInit()
    {
        constexpr int k10MBInBytes = 10 * 1024 * 1024;
        constexpr int k20InCounts  = 20;
        std::string logDir         = "./log";
        // 文件名带进程号：与 UnitTests 并发跑时共用 ./log 不撞车（TestProcess.hpp）。
        std::string logFileName = "livesmoke_" + std::to_string(sb_test::processId());
        sb_test::ensureDirectory(logDir);
        LogWrapper::getInstanceInitialize(logDir, logFileName, k10MBInBytes, k20InCounts);
    }
};

const LogInit g_logInit;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[smoke]   FAIL: %s\n", what);
    }
}

// addMusicWithNotify + future 串行加载；守卫 15s 只防死锁，不是行为判据
// （与 test_live_source.cpp 的 loadPlayList 同形）。
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
        const std::future_status status = future.wait_for(std::chrono::seconds(15));
        if (status != std::future_status::ready) {
            std::fprintf(stderr, "[smoke]   FATAL: addMusicWithNotify timeout: %s\n",
                         it->c_str());
            std::exit(2);
        }
    }
    return playlist;
}

// 本 TU 经 winsock 头链拉入了 windows.h，其 ERROR 宏与 StreamDecoderState 的
// ERROR 成员撞名：作用域内取消宏定义，取别名后立即恢复。
#pragma push_macro("ERROR")
#undef ERROR
namespace {
const StreamDecoderState kDecoderErrorState = StreamDecoderState::ERROR;
}
#pragma pop_macro("ERROR")

// 稳态观察器：边排空 ring 边采样状态 / 错误 / 位置 / 时长（LiveWatch 同形）。
// 另记录状态迁移序列（§10.2：判据输出 = 事件计数 + 回调序列 + 有限时间返回）。
struct SmokeWatch {
    AudioRingBuffer &ring;
    AudioStreamDecoder &decoder;

    bool sawError = false;
    bool sawEosOrSeeking = false;
    bool posRegressed = false;
    uint64_t maxPosMs = 0;
    std::vector<std::string> stateLog;   // 观察到的状态迁移序列（去重连续重复项）
    int errorCodeAtError = -1;           // 首次进入 ERROR 时的错误码值

    SmokeWatch(AudioRingBuffer &r, AudioStreamDecoder &d) : ring(r), decoder(d) { }

    void sample()
    {
        while (ring.availableRead() > 0) {
            char buf[4096];
            const size_t once = std::min<size_t>(sizeof(buf), ring.availableRead());
            if (ring.read(buf, once) == 0) break;
        }
        const StreamDecoderState st = decoder.state();
        const char *name = "ERROR";
        if (st == kDecoderErrorState) {
            sawError = true;
            if (errorCodeAtError < 0) {
                errorCodeAtError = static_cast<int>(decoder.errorCode());
            }
        } else if (st == StreamDecoderState::DECODING) {
            name = "DECODING";
        } else if (st == StreamDecoderState::SEEKING) {
            name = "SEEKING";
            sawEosOrSeeking = true;
        } else if (st == StreamDecoderState::EOS) {
            name = "EOS";
            sawEosOrSeeking = true;
        } else {
            name = "IDLE";
        }
        if (stateLog.empty() || stateLog.back() != name) stateLog.push_back(name);
        const uint64_t p = decoder.positionMs();
        if (p < maxPosMs) posRegressed = true;
        if (p > maxPosMs) maxPosMs = p;
    }
};

// 回调序列输出（§10.2）：打印观察到的状态迁移序列与终局错误码。
void printStates(const char *scenario, const SmokeWatch &watch)
{
    std::fprintf(stderr, "[smoke]   states(%s):", scenario);
    for (std::vector<std::string>::const_iterator it = watch.stateLog.begin();
         it != watch.stateLog.end(); ++it) {
        std::fprintf(stderr, " %s", it->c_str());
    }
    if (watch.errorCodeAtError >= 0) {
        std::fprintf(stderr, " (errorCode=%d)", watch.errorCodeAtError);
    }
    std::fprintf(stderr, "\n");
}

// 稳态窗口：播放 wallSeconds 墙钟时间，逐 tick 采样。返回结束时的观察器。
// 场景时长是场景本身（无限流「不主动结束」的定义域），不是墙钟数值断言。
SmokeWatch steadyPlay(AudioRingBuffer &ring, AudioStreamDecoder &decoder, int wallSeconds)
{
    SmokeWatch watch(ring, decoder);
    const std::chrono::steady_clock::time_point deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(wallSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        watch.sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    watch.sample();
    return watch;
}

void printCounts(const char *scenario, const AudioStreamDecoder &decoder,
                 const MockHttpServer &server, const char *livePath)
{
    std::fprintf(stderr,
                 "[smoke]   counts(%s): reconnectAttempts=%d liveFetches=%lld failCount=%lld\n",
                 scenario, decoder.liveReconnectAttempts(),
                 server.requestCountFor(livePath), server.failCount());
}

} // namespace

int main()
{
    const std::vector<uint8_t> &bytes = fixtureBytes();
    if (bytes.size() <= 64) {
        std::fprintf(stderr, "[smoke] FATAL: fixture too small\n");
        return 2;
    }

    // ── 场景 1：windowed LIVE 稳态 8s —— 无限流不主动结束（§10.2 事件计数正路径）。
    {
        std::fprintf(stderr, "[smoke] scenario 1: windowed LIVE steady play 8s\n");
        MockHttpServer::Config cfg;
        cfg.liveWindowPath = "/live";
        MockHttpServer server(bytes, cfg);
        check(server.port() > 0, "mock server bound");

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live") });
        check(playlist->getMusicCount() == 1, "playlist dispatched");
        check(cb.errors.empty(), "no scan errors");
        check(!cb.currentBufs.empty() && cb.currentBufs.front()->signalProperties.durationMs == 0,
              "durationMs == 0 (LIVE single signal)");

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = cb.currentBufs.front()->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        check(decoder.start(cb.currentBufs.front()->processProperties.extractor.get())
                  == sdk_utils::OK, "decoder started");

        SmokeWatch watch = steadyPlay(ring, decoder, 8);
        check(watch.sawError == false, "no ERROR during 8s steady play");
        check(watch.sawEosOrSeeking == false, "no EOS/SEEKING (infinite stream does not end)");
        check(watch.posRegressed == false, "position monotonic");
        check(watch.maxPosMs >= 1000, "position advanced (supply flowing)");
        check(decoder.durationMs() == 0, "duration stays 0");
        check(decoder.liveReconnectAttempts() == 0, "zero reconnects in steady state");
        check(server.failCount() == 0, "zero injected failures");
        check(decoder.state() == StreamDecoderState::DECODING, "still DECODING at 8s");
        printStates("live-steady", watch);
        printCounts("live-steady", decoder, server, "/live");

        // ── 场景 3 前置：就用这条稳态流做 stop() 有限时间返回（D20 终止路径）。
        std::fprintf(stderr, "[smoke] scenario 3: stop() returns within bounded time\n");
        const uint64_t posAtStop = watch.maxPosMs;
        std::future<void> stopDone = std::async(std::launch::async, [&decoder]() {
            decoder.stop();
        });
        const std::future_status stopStatus = stopDone.wait_for(std::chrono::seconds(5));
        check(stopStatus == std::future_status::ready, "stop() returned within 5s guard");
        check(decoder.state() == StreamDecoderState::IDLE, "state IDLE after stop()");
        check(decoder.positionMs() >= posAtStop, "position not reset by stop");
        printStates("live-stop", watch);
        printCounts("live-stop", decoder, server, "/live");
    }

    // ── 场景 2：ICY 稳态 8s —— 原生 URL 路径，无限 mp3 推流不主动结束（D19）。
    {
        std::fprintf(stderr, "[smoke] scenario 2: ICY steady play 8s\n");
        MockHttpServer::Config cfg;
        cfg.icyStreamPath = "/stream";
        MockHttpServer server(bytes, cfg);
        check(server.port() > 0, "mock server bound");

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/stream") });
        check(playlist->getMusicCount() == 1, "playlist dispatched");
        check(cb.errors.empty(), "no scan errors");

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = cb.currentBufs.front()->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        check(decoder.start(cb.currentBufs.front()->processProperties.extractor.get())
                  == sdk_utils::OK, "decoder started");

        SmokeWatch watch = steadyPlay(ring, decoder, 8);
        check(watch.sawError == false, "no ERROR during 8s steady play");
        check(watch.sawEosOrSeeking == false, "no EOS/SEEKING (infinite stream does not end)");
        check(watch.posRegressed == false, "position monotonic");
        check(watch.maxPosMs >= 1000, "position advanced (supply flowing)");
        check(decoder.state() == StreamDecoderState::DECODING, "still DECODING at 8s");
        printStates("icy-steady", watch);
        printCounts("icy-steady", decoder, server, "/stream");

        decoder.stop();
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "[smoke] ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "[smoke] %d check(s) FAILED\n", g_failures);
    return 1;
}
