// P3-2 门禁（票 #159 / doc/live-source-spec.md §10）：LIVE 重连状态机的事件计数判据。
//
// 职责分界（§10.2 D23）：本套件只收「确定性有限」的三类判据——预算耗尽、断流恢复、
// seek 短路；无限流正路径（不主动结束）与 ICY 稳态播放由独立可执行 LiveSmoke 承接
// （注册构建、不注册 ctest）。事件计数为主，墙钟只作防挂死守卫，不基于墙钟数值断言。
//
// 素材与 mock：窗口化 LIVE 模式（HttpMocks.hpp 的 liveWindowPath）——清单每次取回
// 回 2 段滑动窗口（MEDIA-SEQUENCE 递进），segment 为独立完整 wav（fixture PCM
// 环回切片）。所有流量在 127.0.0.1 进程私有端口，公网 URL 零依赖。
#include "../fixtures/RealMediaFixture.hpp"
#include <AudioRingBuffer.h>
#include <AudioStreamDecoder.h>
#include <MusicPlayList.h>
#include <doctest/doctest.h>
// P3 共享 mock 在所有 sdk 头之后 include：HttpMocks 自带 winsock2，
// wingdi 的 ERROR 宏会污染 StreamDecoderState::ERROR（与 test_http_source.cpp 同序）。
#include "../mocks/HttpMocks.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sb_httpmock;
using namespace sdk;

namespace {

// 回调把扫描阶段登记的「当前曲目属性」与错误码带出来，供断言观察（形态同
// test_http_source.cpp 的 DispatchCallback）。
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

// addMusicWithNotify + future 串行加载；守卫 15s 只防死锁，不是行为判据
// （取值依据见 test_http_source.cpp loadPlayList 注释）。
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
            FAIL("addMusicWithNotify did not complete within 15s: " << *it);
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

// 解码侧观察器：边排空 ring 边采样状态 / 错误 / 位置 / 时长。事件计数判据的数据源，
// 只记「发生过什么」，不做墙钟数值断言（墙钟只出现在各用例的防挂死守卫上）。
struct LiveWatch {
    AudioRingBuffer &ring;
    AudioStreamDecoder &decoder;

    bool sawSeekingOrEos = false;    // DECODING / ERROR 之外的任何状态（短路判据）
    bool sawError = false;           // 进入过 ERROR
    bool errorThenDecoding = false;  // ERROR 之后回到 DECODING（终局不唯一）
    bool durationNonZero = false;    // duration 恒 0 判据
    bool posRegressed = false;       // position 单调不回退判据
    uint64_t maxPosMs = 0;

    // C++11 下带 NSDMI 的类不是聚合，不能用花括号初始化，显式给构造函数。
    LiveWatch(AudioRingBuffer &r, AudioStreamDecoder &d) : ring(r), decoder(d) { }

    void sample()
    {
        while (ring.availableRead() > 0) {
            char buf[4096];
            const size_t once = std::min<size_t>(sizeof(buf), ring.availableRead());
            if (ring.read(buf, once) == 0) break;
        }
        const StreamDecoderState st = decoder.state();
        if (st == kDecoderErrorState) {
            sawError = true;
        } else if (st == StreamDecoderState::DECODING) {
            if (sawError) errorThenDecoding = true;
        } else {
            sawSeekingOrEos = true;
        }
        if (decoder.durationMs() != 0) durationNonZero = true;
        const uint64_t p = decoder.positionMs();
        if (p < maxPosMs) posRegressed = true;
        if (p > maxPosMs) maxPosMs = p;
    }
};

// 轮询直至条件成立；返回 false = 防挂死守卫到点（这不是行为断言，到点即挂死证据）。
template <typename Pred>
bool pollUntil(const Pred &done, int guardMs, LiveWatch *watch = nullptr)
{
    const int stepMs = 5;
    int elapsed = 0;
    while (elapsed <= guardMs) {
        if (watch != nullptr) watch->sample();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
        elapsed += stepMs;
    }
    return false;
}

} // namespace

TEST_SUITE("LiveSourceReconnect")
{
    // 票 #159 事件计数①：断流注入后重开恰好 3 次（预算耗尽）、终局恰好一次
    // FileOpenFailed、期间零额外状态变化、position 停滞不回退、duration 恒 0。
    //
    // 注入面按 playlist 取回序号（seq）锚定：扫描期 find_stream_info 的 reload
    // 次数随素材分析时长浮动，请求总数无法定位「播放期首个 reload」；测试在
    // 扫描完成后按实测 /live 取回数 S 设定窗口 [S,∞)，此后所有 playlist 取回
    // （reload burst + 3 次重开的 open）全部 404。主判据 = 解码器尝试计数仪表
    // （规格 §10.2 取数点，liveReconnectAttempts()）：只统计「真正走到重开一步」
    // 的尝试，FFmpeg read_data 吞错重试的快速 burst 不进该计数，预算破坏
    // （多试第 4 次 / 少试 / 去掉计数）直接变红。mock 侧辅助判据：
    // fails == liveFetches − S —— 注入面外零失败、注入面内每次取回恰一次 404，
    // burst 波动同步改动等号两侧仍守恒；段请求不进 /live 计数（重开全败不碰段）。
    TEST_CASE("live reconnect budget exhaustion ends with exactly one FileOpenFailed")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.liveWindowPath = "/live";
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        REQUIRE(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);   // LIVE 单一信号

        // 断流注入面在扫描完成后设定（见用例头注释）：S = 扫描期 /live 取回数，
        // 首个播放期 reload 的 seq 恰好等于 S。
        const long long scanLiveFetches = server.requestCountFor("/live");
        server.setPlaylistFailWindow((int)scanLiveFetches, 1000000);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);

        LiveWatch watch{ring, decoder};
        // 等终局：预算 ≤12s（3×1s 等待 + 重开窗口），守卫 30s 只防挂死。
        REQUIRE(pollUntil([&watch]() { return watch.sawError; }, 30000, &watch));
        REQUIRE(decoder.errorCode() == ErrorCode::FileOpenFailed);
        CHECK(decoder.errorDetail() == "live source reconnect budget exhausted");
        // 终局唯一：ERROR 之后不得回到 DECODING，期间不得出现 SEEKING / EOS。
        CHECK(watch.errorThenDecoding == false);
        CHECK(watch.sawSeekingOrEos == false);
        CHECK(watch.durationNonZero == false);
        CHECK(watch.posRegressed == false);

        // 事件计数（结构化，见用例头注释）。主判据 = 尝试计数仪表：恰好 3 次。
        CHECK(decoder.liveReconnectAttempts() == 3);
        const long long liveFetches = server.requestCountFor("/live");
        const long long fails = server.failCount();
        CHECK(fails >= 3);
        CHECK(fails == liveFetches - scanLiveFetches);   // 注入面外零失败、面内每次取回恰一次 404
        CHECK(server.requestCountFor("/seg1.ts") == 1);   // 扫描 open 打开、播放读尽
        CHECK(server.requestCountFor("/seg2.ts") == 1);   // 播放期打开一次；重开全败不碰段

        // position 停滞（终局）：ring 排空后位置冻结（解码线程已退出，无新产出）。
        const uint64_t posAtError = watch.maxPosMs;
        std::vector<char> drain(4096);
        while (ring.availableRead() > 0) {
            const size_t once = std::min<size_t>(drain.size(), ring.availableRead());
            if (ring.read(drain.data(), once) == 0) break;
        }
        bool frozen = true;
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (decoder.positionMs() != posAtError) frozen = false;
        }
        CHECK(frozen == true);

        decoder.stop();
    }

    // 票 #159 事件计数②：断流恢复后续播 position 不回退、duration 恒 0、
    // 期间零额外 onError / onStateChanged（状态全程 DECODING，连 ERROR 都不出现）。
    // 注入面 = playlist 取回 seq ∈ [S, S+4)：4 个取回——覆盖 FFmpeg read_data 对
    // reload 失败的内部快速重试 burst（实测 3 次、15ms 间隔），把错误真正上提
    // 到解码器，恢复必须走我们的重连状态机（attempts ≥ 1）而不是 FFmpeg 内部
    // 吞错重试。窗口取 4 对 burst ∈ [1,4] 都安全：重开 fetch 至多吃掉 3 个注入
    // 位，第 3 次尝试必落在窗外成功，不会把预算打穿成 FileOpenFailed。
    TEST_CASE("supply stall recovers via reconnect without callbacks or position regression")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.liveWindowPath = "/live";
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.currentBufs.size() == 1);
        REQUIRE(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);

        // 注入面在扫描完成后设定：窗口 [S, S+4)，S = 扫描期 /live 取回数。
        const long long scanLiveFetches = server.requestCountFor("/live");
        server.setPlaylistFailWindow((int)scanLiveFetches, 4);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);

        LiveWatch watch{ring, decoder};
        // 停滞证据：恰好一次 404（触发面）。恢复窗口期（重开前固定等待 1s 分片轮询）
        // 位置停住，重开成功后续播推进。
        // 等注入窗口填满（burst 3×15ms + 至多一次重开 fetch）：poll 的触发条件
        // 只是 failCount ≥ 1，若立即取值会在 burst 中途拿到 1/2/3——先等计数
        // 恰好填满注入面再采样，消除与 mock 服务线程的时序竞争。
        REQUIRE(pollUntil([&server]() { return server.failCount() >= 4; }, 20000, &watch));
        CHECK(server.failCount() == 4);   // 注入面恰好填满（burst + 至多一次重开 fetch）
        const uint64_t posAtStall = watch.maxPosMs;
        CHECK(watch.sawSeekingOrEos == false);
        CHECK(watch.sawError == false);
        CHECK(watch.durationNonZero == false);
        CHECK(watch.posRegressed == false);

        // 续播证据：位置越过停滞点继续推进（+300ms 的 PCM 是重开后新解出的）。
        const bool resumed = pollUntil([&]() { return watch.maxPosMs >= posAtStall + 300; },
                                       20000, &watch);
        REQUIRE(resumed);
        CHECK(watch.sawSeekingOrEos == false);
        CHECK(watch.sawError == false);
        CHECK(watch.errorThenDecoding == false);
        CHECK(watch.durationNonZero == false);
        CHECK(watch.posRegressed == false);
        CHECK(server.failCount() == 4);   // 恢复后不再有 404：注入面外零失败
        CHECK(server.requestCountFor("/live") >= scanLiveFetches + 5);   // burst + 重开 fetch 确实发生
        CHECK(decoder.liveReconnectAttempts() >= 1);   // 恢复走我们的重连状态机，非 FFmpeg 内部吞错
        CHECK(decoder.state() == StreamDecoderState::DECODING);

        decoder.stop();
    }

    // 票 #159 事件计数③：seek(0) 在 LIVE 上静默短路（#153 D21 实现层）——
    // 不进 SEEKING、不 EOS、position 不回退、零状态变化；duration 恒 0。
    // MusicPlayer 层的 pos > durationMs 空操作公开契约不动（§7），这里补的是
    // pos==0 穿透解码层的实现层判据。稳态供给（无 fail 窗口）。
    TEST_CASE("seek 0 on a live stream is a silent short-circuit")
    {
        const std::vector<uint8_t> &bytes = fixtureBytes();
        REQUIRE(bytes.size() > 64);
        MockHttpServer::Config cfg;
        cfg.liveWindowPath = "/live";
        MockHttpServer server(bytes, cfg);
        REQUIRE(server.port() > 0);

        DispatchCallback cb;
        std::shared_ptr<MusicPlayList> playlist = loadPlayList(cb, { server.url("/live") });
        REQUIRE(playlist->getMusicCount() == 1);
        REQUIRE(cb.errors.empty());
        MusicPropertiesPtr props = cb.currentBufs.front();
        REQUIRE(props->signalProperties.durationMs == 0);

        AudioRingBuffer ring(1 << 20);
        AudioSpec devSpec = props->signalProperties.spec;
        AudioStreamDecoder decoder(&ring, devSpec);
        REQUIRE(decoder.start(props->processProperties.extractor.get()) == sdk_utils::OK);

        LiveWatch watch{ring, decoder};
        // 播到有位置可回退量级后再 seek（供给节奏 ~0.5s 一片，守卫 20s 只防挂死）。
        REQUIRE(pollUntil([&watch]() { return watch.maxPosMs >= 200; }, 20000, &watch));
        const uint64_t posBeforeSeek = watch.maxPosMs;
        const int attemptsBeforeSeek = decoder.liveReconnectAttempts();

        decoder.seekToMs(0);
        decoder.seekToMs(2000);   // 任意目标同判：LIVE 上 seek 一律静默忽略
        REQUIRE(pollUntil([&]() { return watch.maxPosMs >= posBeforeSeek + 300; },
                          20000, &watch));
        CHECK(watch.sawSeekingOrEos == false);   // 不进 SEEKING（短路）、不 EOS
        CHECK(watch.sawError == false);
        CHECK(watch.durationNonZero == false);
        CHECK(watch.posRegressed == false);      // position 不回退
        CHECK(server.failCount() == 0);          // 稳态无断流：seek 不触发重连
        CHECK(decoder.liveReconnectAttempts() == attemptsBeforeSeek);   // 短路不开重连状态机
        CHECK(decoder.state() == StreamDecoderState::DECODING);

        decoder.stop();
    }
}
